#include "app/Window.h"
#include "app/WindowInternal.h"
#include "app/TerminalLinks.h"
#include "core/RenderSignal.h"
#include "util/InputBox.h"
#include "util/Process.h"

#include <ole2.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace liney {

namespace {

std::wstring localFileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring localParentPath(const std::wstring& path) {
    size_t end = path.size();
    while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) --end;
    const size_t slash = path.find_last_of(L"\\/", end == 0 ? 0 : end - 1);
    if (slash == std::wstring::npos) return L".";
    if (slash == 2 && path.size() >= 3 && path[1] == L':')
        return path.substr(0, 3);
    return path.substr(0, slash);
}

std::wstring joinLocalPath(const std::wstring& parent,
                           const std::wstring& name) {
    if (parent.empty()) return name;
    if (parent.back() == L'\\' || parent.back() == L'/') return parent + name;
    return parent + L"\\" + name;
}

std::wstring joinRemotePath(const std::wstring& parent,
                            const std::wstring& name) {
    if (parent.empty() || parent == L"/") return L"/" + name;
    return parent.back() == L'/' ? parent + name : parent + L"/" + name;
}

std::wstring remoteParentPath(const std::wstring& path) {
    if (path.empty() || path == L"/") return L"/";
    size_t end = path.size();
    while (end > 1 && path[end - 1] == L'/') --end;
    const size_t slash = path.find_last_of(L'/', end - 1);
    if (slash == std::wstring::npos || slash == 0) return L"/";
    return path.substr(0, slash);
}

std::wstring sftpSessionKey(const TerminalSession* session) {
    if (!session || !session->context().sshProfile) return {};
    const SshProfile& profile = *session->context().sshProfile;
    return sshProfileTarget(profile) + L":" + std::to_wstring(profile.port) +
           L":" + profile.identityFile;
}

std::wstring win32FileError(const wchar_t* operation) {
    const DWORD code = GetLastError();
    wchar_t message[512]{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        code, 0, message, static_cast<DWORD>(_countof(message)), nullptr);
    std::wstring result = operation;
    if (length != 0) {
        result += L": ";
        result.append(message, length);
        while (!result.empty() &&
               (result.back() == L'\r' || result.back() == L'\n'))
            result.pop_back();
    }
    return result;
}

bool localCopyTree(const std::wstring& source, const std::wstring& destination,
                   std::wstring& error) {
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = win32FileError(L"Could not read source");
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (!CopyFileW(source.c_str(), destination.c_str(), TRUE)) {
            error = win32FileError(L"Could not copy file");
            return false;
        }
        return true;
    }

    if (!CreateDirectoryW(destination.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        error = win32FileError(L"Could not create folder");
        return false;
    }
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW((source + L"\\*").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        error = win32FileError(L"Could not enumerate folder");
        return false;
    }
    bool ok = true;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring childSource = joinLocalPath(source, name);
        const std::wstring childDestination = joinLocalPath(destination, name);
        if (!localCopyTree(childSource, childDestination, error)) {
            ok = false;
            break;
        }
    } while (FindNextFileW(handle, &data));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) {
        error = win32FileError(L"Could not enumerate folder");
        ok = false;
    }
    FindClose(handle);
    return ok;
}

UINT preferredDropEffectFormat() {
    static const UINT format = RegisterClipboardFormatW(
        CFSTR_PREFERREDDROPEFFECT);
    return format;
}

HGLOBAL createFileDropHandle(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return nullptr;
    size_t chars = 1;  // The final list terminator.
    for (const std::wstring& path : paths) {
        if (path.empty() || path.size() > (SIZE_MAX / sizeof(wchar_t)) - chars - 1)
            return nullptr;
        chars += path.size() + 1;
    }
    if (chars > (SIZE_MAX - sizeof(DROPFILES)) / sizeof(wchar_t))
        return nullptr;
    const SIZE_T bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!handle) return nullptr;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(handle));
    if (!drop) {
        GlobalFree(handle);
        return nullptr;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t* destination = reinterpret_cast<wchar_t*>(
        reinterpret_cast<BYTE*>(drop) + drop->pFiles);
    for (const std::wstring& path : paths) {
        memcpy(destination, path.c_str(), path.size() * sizeof(wchar_t));
        destination += path.size();
        *destination++ = L'\0';
    }
    *destination = L'\0';
    GlobalUnlock(handle);
    return handle;
}

HGLOBAL createPreferredDropEffectHandle() {
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                                 sizeof(DWORD));
    if (!handle) return nullptr;
    auto* effect = static_cast<DWORD*>(GlobalLock(handle));
    if (!effect) {
        GlobalFree(handle);
        return nullptr;
    }
    *effect = DROPEFFECT_COPY;
    GlobalUnlock(handle);
    return handle;
}

bool isFileDropFormat(const FORMATETC& format) {
    return format.cfFormat == CF_HDROP &&
           (format.tymed & TYMED_HGLOBAL) != 0 &&
           format.dwAspect == DVASPECT_CONTENT && format.lindex == -1;
}

bool isPreferredDropEffectFormat(const FORMATETC& format) {
    return preferredDropEffectFormat() != 0 &&
           format.cfFormat == preferredDropEffectFormat() &&
           (format.tymed & TYMED_HGLOBAL) != 0 &&
           format.dwAspect == DVASPECT_CONTENT && format.lindex == -1;
}

class FileDataObject final : public IDataObject {
public:
    explicit FileDataObject(std::wstring path) : path_(std::move(path)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDataObject)
            *result = static_cast<IDataObject*>(this);
        if (!*result) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG references = static_cast<ULONG>(
            InterlockedDecrement(&references_));
        if (references == 0) delete this;
        return references;
    }

    STDMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium) return E_POINTER;
        ZeroMemory(medium, sizeof(*medium));
        if (isFileDropFormat(*format)) {
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = createFileDropHandle({path_});
        } else if (isPreferredDropEffectFormat(*format)) {
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = createPreferredDropEffectHandle();
        } else {
            return DV_E_FORMATETC;
        }
        return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    STDMETHODIMP QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (isFileDropFormat(*format) || isPreferredDropEffectFormat(*format))
            return S_OK;
        if (format->cfFormat == CF_HDROP ||
            format->cfFormat == preferredDropEffectFormat())
            return DV_E_TYMED;
        return DV_E_FORMATETC;
    }

    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* source,
                                       FORMATETC* destination) override {
        if (!source || !destination) return E_POINTER;
        *destination = *source;
        destination->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP EnumFormatEtc(DWORD direction,
                               IEnumFORMATETC** enumerator) override {
        if (!enumerator) return E_POINTER;
        *enumerator = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC formats[2]{};
        formats[0].cfFormat = CF_HDROP;
        formats[0].dwAspect = DVASPECT_CONTENT;
        formats[0].lindex = -1;
        formats[0].tymed = TYMED_HGLOBAL;
        UINT count = 1;
        if (preferredDropEffectFormat() != 0) {
            formats[count].cfFormat = preferredDropEffectFormat();
            formats[count].dwAspect = DVASPECT_CONTENT;
            formats[count].lindex = -1;
            formats[count].tymed = TYMED_HGLOBAL;
            ++count;
        }
        return SHCreateStdEnumFmtEtc(count, formats, enumerator);
    }

    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }

    STDMETHODIMP EnumDAdvise(IEnumSTATDATA** enumerator) override {
        if (enumerator) *enumerator = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~FileDataObject() = default;

    LONG references_ = 1;
    std::wstring path_;
};

class FileDropSource final : public IDropSource {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropSource)
            *result = static_cast<IDropSource*>(this);
        if (!*result) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        const ULONG references = static_cast<ULONG>(
            InterlockedDecrement(&references_));
        if (references == 0) delete this;
        return references;
    }

    STDMETHODIMP QueryContinueDrag(BOOL escapePressed,
                                   DWORD keyState) override {
        if (escapePressed) return DRAGDROP_S_CANCEL;
        if ((keyState & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }

    STDMETHODIMP GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ~FileDropSource() = default;

    LONG references_ = 1;
};

bool localDeletePath(const std::wstring& path, std::wstring& error) {
    std::wstring paths = path;
    paths.push_back(L'\0');
    paths.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = paths.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
                       FOF_NOERRORUI | FOF_SILENT;
    const int result = SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted) {
        error = L"Could not delete the selected path.";
        return false;
    }
    return true;
}

bool safeFileName(const std::wstring& name) {
    return !name.empty() && name != L"." && name != L".." &&
           name.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos;
}

bool samePath(const std::wstring& left, const std::wstring& right,
              bool remote) {
    if (remote) return left == right;
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool pathContains(const std::wstring& parent, const std::wstring& child,
                  bool remote) {
    if (samePath(parent, child, remote)) return true;
    std::wstring prefix = parent;
    while (prefix.size() > 1 &&
           (prefix.back() == L'\\' || prefix.back() == L'/')) prefix.pop_back();
    if (!(remote && prefix == L"/")) prefix += remote ? L"/" : L"\\";
    if (child.size() < prefix.size()) return false;
    const std::wstring childPrefix = child.substr(0, prefix.size());
    if (remote) return childPrefix == prefix;
    return _wcsicmp(childPrefix.c_str(), prefix.c_str()) == 0;
}

bool isOpenableTerminalUrl(const std::wstring& url) {
    std::wstring lower = url;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    return lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"mailto:", 0) == 0 ||
           lower.rfind(L"file://", 0) == 0;
}

bool openTerminalUrl(HWND hwnd, const std::wstring& url) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
               hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) >
           32;
}

}  // namespace

bool Window::setWindowsFileClipboard(const std::wstring& path) {
    HGLOBAL drop = createFileDropHandle({path});
    if (!drop) return false;
    HGLOBAL preferredEffect = createPreferredDropEffectHandle();
    if (!OpenClipboard(hwnd_)) {
        GlobalFree(drop);
        if (preferredEffect) GlobalFree(preferredEffect);
        return false;
    }
    bool copied = false;
    if (EmptyClipboard()) {
        copied = SetClipboardData(CF_HDROP, drop) != nullptr;
        if (!copied) {
            GlobalFree(drop);
        } else if (preferredEffect &&
                   SetClipboardData(preferredDropEffectFormat(),
                                    preferredEffect) == nullptr) {
            GlobalFree(preferredEffect);
        }
    } else {
        GlobalFree(drop);
        if (preferredEffect) GlobalFree(preferredEffect);
    }
    CloseClipboard();
    return copied;
}

void Window::startExternalFileDrag(const std::wstring& path) {
    if (path.empty() ||
        GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        showToast(L"The selected item is no longer available.", true);
        return;
    }

    auto* data = new (std::nothrow) FileDataObject(path);
    auto* source = new (std::nothrow) FileDropSource();
    if (!data || !source) {
        if (data) data->Release();
        if (source) source->Release();
        showToast(L"Could not start the Windows file drag.", true);
        return;
    }

    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    source->Release();
    data->Release();
    if (FAILED(result) && result != DRAGDROP_S_CANCEL) {
        showToast(L"Could not start the Windows file drag.", true);
    }
    markRenderDirty();
}

void Window::invalidateFileList() {
    listedDir_.clear();
    remoteListedRequestKey_.clear();
    remoteFileError_.clear();
    remoteRetryAt_ = 0;
    remoteRetryCount_ = 0;
    fileScrollOffset_ = 0.0f;
    markRenderDirty();
}

void Window::activateFileRow(bool isDir, const std::wstring& path) {
    const TerminalSession* session = activeSession();
    const bool remote = session && session->isSsh() &&
        session->context().sshProfile.has_value();
    if (isDir) {
        if (remote) remoteBrowsePath_ = path;
        else browsePath_ = path;
        fileScrollOffset_ = 0.0f;
        markRenderDirty();
        return;
    }
    const std::wstring name = localFileName(path);
    const std::wstring insertion = name.find(L' ') != std::wstring::npos
        ? L"\"" + name + L"\" " : name + L" ";
    sendUtf16(insertion.c_str(), insertion.size());
}

void Window::requestRemoteFileOperation(SshFileOperationKind kind,
                                         const std::wstring& source,
                                         const std::wstring& destination,
                                         const std::wstring& message) {
    TerminalSession* session = activeSession();
    if (!session || !session->isSsh() ||
        !session->context().sshProfile.has_value()) {
        showToast(L"The active SSH session is not available.", true);
        return;
    }
    if (remoteFileOperationRequestId_ != 0) {
        showToast(L"An SFTP file operation is already in progress.", true);
        return;
    }
    const SshFileOperationRequestId id = session->requestSftpFileOperation(
        kind, source, destination);
    if (id == 0) {
        showToast(L"The active SFTP session is not available.", true);
        return;
    }
    remoteFileOperationRequestId_ = id;
    remoteFileOperationSession_ = session;
    remoteFileOperationSessionKey_ = sftpSessionKey(session);
    remoteFileOperationMessage_ = message;
    showToast(message);
    markRenderDirty();
}

void Window::pollRemoteFileOperation() {
    if (remoteFileOperationRequestId_ == 0 || !remoteFileOperationSession_)
        return;
    bool stillAlive = false;
    for (const auto& tab : tabs_) {
        for (Pane* pane : tab->leaves()) {
            if (pane && pane->session.get() == remoteFileOperationSession_) {
                stillAlive = true;
                break;
            }
        }
        if (stillAlive) break;
    }
    if (!stillAlive) {
        remoteFileOperationRequestId_ = 0;
        remoteFileOperationSession_ = nullptr;
        remoteFileOperationSessionKey_.clear();
        remoteFileOperationMessage_.clear();
        return;
    }
    const auto completed = remoteFileOperationSession_->takeSftpFileOperationResult(
        remoteFileOperationRequestId_);
    if (!completed) return;
    const std::wstring message = remoteFileOperationMessage_;
    remoteFileOperationRequestId_ = 0;
    remoteFileOperationSession_ = nullptr;
    remoteFileOperationSessionKey_.clear();
    remoteFileOperationMessage_.clear();
    if (!completed->ok) {
        showToast(completed->error.empty() ? L"SFTP file operation failed."
                                          : completed->error,
                  true);
        return;
    }
    invalidateFileList();
    showToast(message.empty() ? L"SFTP file operation complete."
                              : message + L" complete.");
}

void Window::finishFileDrag(int xi, int yi) {
    const bool pending = fileDragPending_;
    const bool active = fileDragActive_;
    const bool remote = fileDragRemote_;
    const bool isDir = fileDragIsDir_;
    const std::wstring source = fileDragPath_;
    const std::wstring sessionKey = fileDragSessionKey_;
    fileDragPending_ = false;
    fileDragActive_ = false;
    fileDragPath_.clear();
    fileDragSessionKey_.clear();
    ReleaseCapture();
    if (!pending) return;

    if (!active) {
        activateFileRow(isDir, source);
        return;
    }

    Rect leftBar, rightPanel, tabBar, panes;
    regions(leftBar, rightPanel, tabBar, panes);
    if (!rightPanel.contains(static_cast<float>(xi), static_cast<float>(yi))) {
        showToast(L"Drop the item on a folder in the file browser.", true);
        return;
    }
    TerminalSession* session = activeSession();
    const bool currentRemote = session && session->isSsh() &&
        session->context().sshProfile.has_value();
    if (currentRemote != remote ||
        (remote && sessionKey != remoteSessionKey_)) {
        showToast(L"The source and destination must use the same file browser.",
                  true);
        return;
    }

    std::wstring destinationDirectory = currentRemote
        ? remoteBrowsePath_ : browsePath_;
    for (const SidebarRow& row : sidebarRows_) {
        if (row.rect.contains(static_cast<float>(xi), static_cast<float>(yi)) &&
            row.kind == RowKind::FileDir) {
            destinationDirectory = row.path;
            break;
        }
    }
    if (destinationDirectory.empty()) {
        showToast(L"The destination folder is not available.", true);
        return;
    }
    const std::wstring destination = currentRemote
        ? joinRemotePath(destinationDirectory, localFileName(source))
        : joinLocalPath(destinationDirectory, localFileName(source));
    if (pathContains(source, destination, currentRemote)) {
        showToast(L"A folder cannot be moved into itself.", true);
        return;
    }
    if (samePath(source, destination, currentRemote)) return;

    if (currentRemote) {
        requestRemoteFileOperation(SshFileOperationKind::Move, source,
                                   destination, L"Moving remote item…");
        return;
    }
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_COPY_ALLOWED)) {
        showToast(win32FileError(L"Could not move item"), true);
        return;
    }
    invalidateFileList();
    showToast(L"Item moved.");
}

void Window::openFileMenu(int xi, int yi) {
    Rect leftBar, rightPanel, tabBar, panes;
    regions(leftBar, rightPanel, tabBar, panes);
    if (!filesPanelVisible_ || !rightPanel.contains(static_cast<float>(xi),
                                                     static_cast<float>(yi)))
        return;

    TerminalSession* session = activeSession();
    const bool remote = session && session->isSsh() &&
        session->context().sshProfile.has_value();
    std::wstring selectedPath;
    bool selectedIsDir = false;
    for (const SidebarRow& row : sidebarRows_) {
        if (!row.rect.contains(static_cast<float>(xi), static_cast<float>(yi)))
            continue;
        if (row.kind == RowKind::FileDir || row.kind == RowKind::FileEntry) {
            selectedPath = row.path;
            selectedIsDir = row.kind == RowKind::FileDir;
        }
        break;
    }

    const std::wstring destinationDirectory = selectedIsDir
        ? selectedPath : (remote ? remoteBrowsePath_ : browsePath_);
    const bool clipboardMatches = !fileClipboardPath_.empty() &&
        fileClipboardRemote_ == remote &&
        (!remote || fileClipboardSessionKey_ == sftpSessionKey(session));
    const bool canPaste = clipboardMatches && !destinationDirectory.empty();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (selectedPath.empty() ? MF_GRAYED : 0),
                200, L"Copy");
    AppendMenuW(menu, MF_STRING | (canPaste ? 0 : MF_GRAYED), 201, L"Paste");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (selectedPath.empty() ? MF_GRAYED : 0),
                202, L"Rename");
    AppendMenuW(menu, MF_STRING | (selectedPath.empty() ? MF_GRAYED : 0),
                203, L"Delete");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 204, L"Refresh");
    POINT point{xi, yi};
    ClientToScreen(hwnd_, &point);
    const int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      point.x, point.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (action == 0) return;

    if (action == 200) {
        fileClipboardPath_ = selectedPath;
        fileClipboardRemote_ = remote;
        fileClipboardIsDir_ = selectedIsDir;
        fileClipboardSessionKey_ = remote ? sftpSessionKey(session) : L"";
        if (!remote && !setWindowsFileClipboard(selectedPath)) {
            showToast(L"File copied to Liney, but Windows clipboard access is busy.",
                      true);
        } else {
            showToast(remote ? L"File copied to the Liney file clipboard."
                             : L"File copied to the Windows clipboard.");
        }
        return;
    }

    if (action == 201 && canPaste) {
        const std::wstring name = localFileName(fileClipboardPath_);
        const std::wstring destination = remote
            ? joinRemotePath(destinationDirectory, name)
            : joinLocalPath(destinationDirectory, name);
        if (pathContains(fileClipboardPath_, destination, remote)) {
            showToast(L"A folder cannot be pasted into itself.", true);
            return;
        }
        if (remote) {
            requestRemoteFileOperation(SshFileOperationKind::Copy,
                                       fileClipboardPath_, destination,
                                       L"Pasting remote item…");
            return;
        }
        if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
            showToast(L"The destination already contains an item with that name.",
                      true);
            return;
        }
        std::wstring error;
        if (!localCopyTree(fileClipboardPath_, destination, error)) {
            showToast(error.empty() ? L"Could not paste item." : error, true);
            return;
        }
        invalidateFileList();
        showToast(L"Item pasted.");
        return;
    }

    if (action == 204) {
        invalidateFileList();
        return;
    }

    if (selectedPath.empty()) return;
    if (action == 202) {
        const std::wstring oldName = localFileName(selectedPath);
        const std::wstring newName = inputBox(
            hwnd_, L"Rename", L"New name:", oldName);
        if (newName.empty() || newName == oldName) return;
        if (!safeFileName(newName)) {
            showToast(L"That is not a valid file name.", true);
            return;
        }
        const std::wstring destination = remote
            ? joinRemotePath(remoteParentPath(selectedPath), newName)
            : joinLocalPath(localParentPath(selectedPath), newName);
        if (remote) {
            requestRemoteFileOperation(SshFileOperationKind::Rename,
                                       selectedPath, destination,
                                       L"Renaming remote item…");
        } else if (!MoveFileExW(selectedPath.c_str(), destination.c_str(),
                                MOVEFILE_COPY_ALLOWED)) {
            showToast(win32FileError(L"Could not rename item"), true);
        } else {
            invalidateFileList();
            showToast(L"Item renamed.");
        }
        return;
    }

    if (action == 203) {
        const std::wstring prompt = L"Delete this item?\n\n" + selectedPath;
        if (MessageBoxW(hwnd_, prompt.c_str(), L"Delete item",
                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
        if (remote) {
            requestRemoteFileOperation(SshFileOperationKind::Delete,
                                       selectedPath, L"",
                                       L"Deleting remote item…");
        } else {
            std::wstring error;
            if (!localDeletePath(selectedPath, error)) {
                showToast(error.empty() ? L"Could not delete item." : error,
                          true);
            } else {
                invalidateFileList();
                showToast(L"Item deleted.");
            }
        }
    }
}

void Window::onMouseDown(int xi, int yi) {
    const float x = static_cast<float>(xi), y = static_cast<float>(yi);
    Rect leftBar, rightPanel, tabBar, panes;
    regions(leftBar, rightPanel, tabBar, panes);

    if (sidebarResizeRect_.contains(x, y)) {
        panelResize_ = PanelResize::Sidebar;
        SetCapture(hwnd_);
        return;
    }
    if (filesResizeRect_.contains(x, y)) {
        panelResize_ = PanelResize::Files;
        SetCapture(hwnd_);
        return;
    }

    if ((sidebarVisible_ && leftBar.contains(x, y)) ||
        (filesPanelVisible_ && rightPanel.contains(x, y))) {
        if (sidebarVisible_ && workspaceAddRect_.contains(x, y)) {
            addWorkspaceFolder();
            return;
        }
        if (filesPanelVisible_ && rightPanel.contains(x, y)) {
            if (fileScrollThumbRect_.contains(x, y)) {
                fileScrollDragging_ = true;
                fileScrollDragOffset_ = y - fileScrollThumbRect_.y;
                SetCapture(hwnd_);
                return;
            }
            if (fileScrollTrackRect_.contains(x, y)) {
                const float page = std::max(metrics_.rowH() * 4.0f,
                                            fileScrollMaxOffset_ * 0.8f);
                fileScrollOffset_ = std::clamp(
                    fileScrollOffset_ + (y < fileScrollThumbRect_.y
                                             ? -page : page),
                    0.0f, fileScrollMaxOffset_);
                markRenderDirty();
                return;
            }
            for (const auto& breadcrumb : fileBreadcrumbs_) {
                if (!breadcrumb.first.contains(x, y)) continue;
                const bool remote = activeSession() &&
                    activeSession()->context().role == SessionRole::Ssh &&
                    activeSession()->context().sshProfile.has_value();
                if (remote)
                    remoteBrowsePath_ = breadcrumb.second;
                else
                    browsePath_ = breadcrumb.second;
                fileScrollOffset_ = 0.0f;
                markRenderDirty();
                return;
            }
            const TerminalSession* session = activeSession();
            const bool remote = session && session->isSsh() &&
                session->context().sshProfile.has_value();
            for (const SidebarRow& row : sidebarRows_) {
                if (!row.rect.contains(x, y)) continue;
                if (row.kind == RowKind::FileDir ||
                    row.kind == RowKind::FileEntry) {
                    fileDragPending_ = true;
                    fileDragActive_ = false;
                    fileDragRemote_ = remote;
                    fileDragIsDir_ = row.kind == RowKind::FileDir;
                    fileDragPath_ = row.path;
                    fileDragSessionKey_ = remote ? sftpSessionKey(session) : L"";
                    fileDragStart_ = {xi, yi};
                    SetCapture(hwnd_);
                    return;
                }
            }
        }
        for (const SidebarRow& row : sidebarRows_) {
            if (!row.rect.contains(x, y)) continue;
            if (row.kind == RowKind::ArchiveHeader) {
                archiveExpanded_ = !archiveExpanded_;
                markRenderDirty();
                return;
            }
            if (row.kind == RowKind::SerialHeader) {
                serialExpanded_ = !serialExpanded_;
                markRenderDirty();
                return;
            }
            if (row.kind == RowKind::SshHeader) {
                sshExpanded_ = !sshExpanded_;
                markRenderDirty();
                return;
            }
            if (row.kind == RowKind::RepoHeader &&
                row.actionRect.contains(x, y)) {
                auto& repos = workspace_.repos();
                if (row.repo >= 0 && row.repo < static_cast<int>(repos.size()))
                    toggleProjectArchive(repos[row.repo]);
                return;
            }
            if (row.kind == RowKind::RecentProject) {
                if (GetFileAttributesW(row.path.c_str()) !=
                    INVALID_FILE_ATTRIBUTES) {
                    rememberRecentProject(row.path);
                    openWorkspaceSession(row.path, row.path);
                } else {
                    showToast(L"Recent project is no longer available", true);
                }
                return;
            }
            switch (row.kind) {
            case RowKind::RepoHeader: {
                auto& repos = workspace_.repos();
                if (row.repo < 0 || row.repo >= static_cast<int>(repos.size())) return;
                Repo& repo = repos[row.repo];
                if (repo.isGit() && !row.archived) {
                    repo.expanded = !repo.expanded;
                    if (repo.expanded) workspace_.loadWorktrees(repo);
                } else {
                    rememberRecentProject(repo.path);
                    openWorkspaceSession(repo.path, repo.path);
                }
                break;
            }
            case RowKind::Worktree: {
                auto& repos = workspace_.repos();
                if (row.repo < 0 || row.repo >= static_cast<int>(repos.size())) return;
                Repo& repo = repos[row.repo];
                if (row.worktree >= 0 &&
                    row.worktree < static_cast<int>(repo.worktrees.size())) {
                    const Worktree& wt = repo.worktrees[row.worktree];
                    openWorkspaceSession(wt.path, repo.path, wt.path);
                }
                break;
            }
            case RowKind::FileUp: {
                const bool remote = activeSession() &&
                    activeSession()->context().role == SessionRole::Ssh &&
                    activeSession()->context().sshProfile.has_value();
                if (remote) {
                    if (remoteBrowsePath_ == L"/") break;
                    if (remoteBrowsePath_.empty() || remoteBrowsePath_ == L".") {
                        remoteBrowsePath_ = L"/";
                        fileScrollOffset_ = 0.0f;
                        markRenderDirty();
                        break;
                    }
                    const size_t s = remoteBrowsePath_.find_last_of(L'/');
                    if (s == std::wstring::npos)
                        remoteBrowsePath_.clear();
                    else if (s == 0)
                        remoteBrowsePath_ = L"/";
                    else
                        remoteBrowsePath_ = remoteBrowsePath_.substr(0, s);
                } else {
                    const size_t s = browsePath_.find_last_of(L"\\/");
                    if (s != std::wstring::npos)
                        browsePath_ = browsePath_.substr(0, s);
                }
                fileScrollOffset_ = 0.0f;
                markRenderDirty();
                break;
            }
            case RowKind::FileDir:
                if (activeSession() &&
                    activeSession()->context().role == SessionRole::Ssh &&
                    activeSession()->context().sshProfile.has_value())
                    remoteBrowsePath_ = row.path;
                else
                    browsePath_ = row.path;  // navigate the panel into the directory
                break;
            case RowKind::FileEntry: {
                // Insert the filename into the focused pane (quote if needed).
                std::wstring name = row.path;
                size_t s = name.find_last_of(L"\\/");
                if (s != std::wstring::npos) name = name.substr(s + 1);
                std::wstring ins = name.find(L' ') != std::wstring::npos
                                       ? L"\"" + name + L"\" "
                                       : name + L" ";
                sendUtf16(ins.c_str(), ins.size());
                break;
            }
            case RowKind::SshHost:
                if (row.repo >= 0 && row.repo < static_cast<int>(sshHosts_.size())) {
                    const SshProfile& profile = sshHosts_[row.repo];
                    SessionContext context;
                    context.role = SessionRole::Ssh;
                    context.taskName = profile.name;
                    context.sshProfile = profile;
                    newTabShell(buildSshCommand(profile), homeDir(), context);
                }
                break;
            case RowKind::SerialPort:
                if (row.repo >= 0 &&
                    row.repo < static_cast<int>(serialPorts_.size()))
                    newTabSerial(serialPorts_[row.repo]);
                break;
            case RowKind::Agent:
                if (row.repo >= 0 && row.repo < static_cast<int>(agents_.size()))
                    if (TerminalSession* session = newTabShell(
                            agents_[row.repo].command, agents_[row.repo].cwd)) {
                        SessionContext context;
                        context.role = SessionRole::Agent;
                        context.agentName = agents_[row.repo].name;
                        context.taskName = agents_[row.repo].name;
                        context.worktreePath = agents_[row.repo].cwd;
                        context.testCommand = agents_[row.repo].testCommand;
                        session->setContext(std::move(context));
                    }
                break;
            }
            return;
        }
        return;
    }

    if (tabBar.contains(x, y)) {
        if (sidebarToggleRect_.contains(x, y)) {
            setSidebarVisible(!sidebarVisible_);
            return;
        }
        if (menuButtonRect_.contains(x, y)) { openMainMenu(); return; }
        if (openButtonRect_.contains(x, y)) { openDirectoryMenu(); return; }
        if (awakeButtonRect_.contains(x, y)) { openKeepAwakeMenu(); return; }
        if (tabOverflowRect_.contains(x, y)) {
            openTabOverflowMenu(xi, yi);
            return;
        }
        if (plusRect_.contains(x, y)) {
            newTab(activeSession() ? activeSession()->cwd() : homeDir());
            return;
        }
        // A hit on a tab's × closes it (with a confirm if it's running a
        // command). Checked before the tab body so it never starts a drag.
        for (size_t i = 0; i < tabCloseRects_.size(); ++i) {
            if (tabCloseRects_[i].contains(x, y)) {
                closeTabConfirming(i);
                return;
            }
        }
        for (size_t i = 0; i < tabRects_.size(); ++i) {
            if (tabRects_[i].contains(x, y)) {
                clearSelection();
                activeTab_ = i;
                tabDragIndex_ = static_cast<int>(i);  // start a potential reorder
                SetCapture(hwnd_);
                updateTitle();
                return;
            }
        }
        return;
    }

    if (panes.contains(x, y)) {
        if (welcomeVisible_ && welcomeOpenRect_.contains(x, y)) {
            welcomeVisible_ = false;
            addWorkspaceFolder();
            return;
        }
        for (const auto& breadcrumb : fileBreadcrumbs_) {
            if (breadcrumb.first.contains(x, y)) {
                if (activeSession() &&
                    activeSession()->context().role == SessionRole::Ssh &&
                    activeSession()->context().sshProfile.has_value())
                    remoteBrowsePath_ = breadcrumb.second;
                else
                    browsePath_ = breadcrumb.second;
                fileScrollOffset_ = 0.0f;
                markRenderDirty();
                return;
            }
        }
        if (welcomeVisible_ && welcomePaletteRect_.contains(x, y)) {
            welcomeVisible_ = false;
            openCommandPalette();
            return;
        }
        if (paneCloseRect_.contains(x, y)) {
            closeActivePaneConfirming();
            return;
        }
        // Clicks on the floating find bar shouldn't start a text selection.
        if (findActive_ && findBarRect_.contains(x, y)) return;
        Tab* t = activeTab();
        if (!t) return;
        // A click near a split divider starts a resize drag instead of a select.
        if (Pane* divider = t->splitDividerAt(x, y, 4.0f)) {
            dragDivider_ = divider;
            SetCapture(hwnd_);
            return;
        }
        Pane* leaf = t->hitTest(x, y);
        if (!leaf) return;
        t->setActive(leaf);

        // Apps that track the mouse (vim :set mouse=a, htop, mc…) get the
        // click; hold Shift to make a local selection instead.
        int cx = 0, cy = 0;
        if (!paneCellAt(leaf, xi, yi, cx, cy)) return;

        // OSC 8 links and plain HTTP(S) URLs share the same deliberate link
        // interaction: Ctrl+click opens immediately; a normal click exposes
        // Copy / Open without accidentally starting terminal selection.
        std::vector<TerminalUrlHit> urlHits;
        const TerminalUrlHit* detected = leaf->session
            ? terminalUrlAt(leaf->session->grid(), cy, cx, urlHits)
            : nullptr;
        const std::wstring oscUri = leaf->session
            ? leaf->session->hyperlinkAt(cx, cy) : L"";
        const std::wstring uri = oscUri.empty()
            ? (detected ? detected->url : L"") : oscUri;
        if (!uri.empty()) {
            if (keyDown(VK_CONTROL)) {
                if (isOpenableTerminalUrl(uri)) {
                    if (!openTerminalUrl(hwnd_, uri))
                        showToast(L"Could not open link", true);
                } else {
                    showBalloon(L"Liney", L"Blocked an unsafe hyperlink protocol");
                }
                return;
            }
            if (detected || isOpenableTerminalUrl(uri)) {
                openTerminalUrlMenu(uri, xi, yi);
                return;
            }
        }

        if (forwardMouse(0 /*press*/, 1 /*left*/, xi, yi)) return;

        // A third press shortly after a double-click on the same row escalates
        // to whole-line selection (double-click already selected the word).
        if (clickStreak_ == 2 && leaf == selPane_ && cy == lastClickCY_ &&
            GetTickCount() - lastClickTick_ <= GetDoubleClickTime()) {
            clickStreak_ = 3;
            lastClickTick_ = GetTickCount();
            selectLineAt(leaf, cy);
            maybeCopyOnSelect();
            return;
        }

        // Begin a selection drag from this cell (a plain click selects nothing
        // until the mouse leaves the cell — WM_MOUSEMOVE fires spuriously on
        // clicks). The anchor is buffer-tracked by the core.
        clickStreak_ = 1;
        selecting_ = true;
        selDragged_ = false;
        selDragCX_ = cx;
        selDragCY_ = cy;
        if (selPane_ && selPane_ != leaf && selPane_->session)
            selPane_->session->selectionClear();
        selPane_ = leaf;
        if (leaf->session) leaf->session->selectionBegin(cx, cy);
        SetCapture(hwnd_);
    }
}

void Window::onMouseDoubleClick(int xi, int yi) {
    const float x = static_cast<float>(xi), y = static_cast<float>(yi);
    Rect leftBar, rightPanel, tabBar, panes;
    regions(leftBar, rightPanel, tabBar, panes);
    if (sidebarVisible_ && leftBar.contains(x, y)) {
        // A single project click is navigation and reuses its existing
        // terminal. A double-click is the explicit "always create another"
        // action for project/worktree sessions.
        for (const SidebarRow& row : sidebarRows_) {
            if (!row.rect.contains(x, y)) continue;
            if (row.kind == RowKind::RecentProject) {
                if (GetFileAttributesW(row.path.c_str()) !=
                    INVALID_FILE_ATTRIBUTES) {
                    rememberRecentProject(row.path);
                    openWorkspaceSession(row.path, row.path, L"", true);
                } else {
                    showToast(L"Recent project is no longer available", true);
                }
                return;
            }
            if (row.kind == RowKind::RepoHeader) {
                auto& repos = workspace_.repos();
                if (row.repo < 0 || row.repo >= static_cast<int>(repos.size()))
                    return;
                const Repo& repo = repos[row.repo];
                rememberRecentProject(repo.path);
                openWorkspaceSession(repo.path, repo.path,
                                     repo.isGit() ? repo.path : L"", true);
                return;
            }
            if (row.kind == RowKind::Worktree) {
                auto& repos = workspace_.repos();
                if (row.repo < 0 || row.repo >= static_cast<int>(repos.size()))
                    return;
                const Repo& repo = repos[row.repo];
                if (row.worktree < 0 ||
                    row.worktree >= static_cast<int>(repo.worktrees.size()))
                    return;
                const Worktree& wt = repo.worktrees[row.worktree];
                openWorkspaceSession(wt.path, repo.path, wt.path, true);
                return;
            }
        }
    }
    if (tabBar.contains(x, y)) {
        // Double-click empty tab-strip space opens a new tab (common convention);
        // on a tab / + / ☰ it's just a click.
        bool onTab = false;
        for (const Rect& tr : tabRects_) if (tr.contains(x, y)) { onTab = true; break; }
        const bool onToolbar = menuButtonRect_.contains(x, y) ||
            openButtonRect_.contains(x, y) || awakeButtonRect_.contains(x, y);
        if (!onTab && !plusRect_.contains(x, y) &&
            !tabOverflowRect_.contains(x, y) &&
            !sidebarToggleRect_.contains(x, y) && !onToolbar)
            newTab(activeSession() ? activeSession()->cwd() : homeDir());
        else
            onMouseDown(xi, yi);
        return;
    }
    if (!panes.contains(x, y)) { onMouseDown(xi, yi); return; }  // other chrome: plain click
    Tab* t = activeTab();
    if (!t) return;
    Pane* leaf = t->hitTest(x, y);
    if (!leaf) return;
    t->setActive(leaf);
    // The second press of a double-click arrives here instead of BUTTONDOWN;
    // mouse-tracking apps still just get the press.
    if (forwardMouse(0 /*press*/, 1 /*left*/, xi, yi)) return;
    int cx = 0, cy = 0;
    if (!paneCellAt(leaf, xi, yi, cx, cy)) return;
    selectWordAt(leaf, cx, cy);
    maybeCopyOnSelect();
    // Arm triple-click detection: a further press on this row escalates to line.
    clickStreak_ = 2;
    lastClickTick_ = GetTickCount();
    lastClickCY_ = cy;
}

void Window::onMouseDownRight(int xi, int yi) {
    const float x = static_cast<float>(xi), y = static_cast<float>(yi);
    Rect leftBar, rightPanel, tabBar, panes;
    regions(leftBar, rightPanel, tabBar, panes);

    // The workspace + keeps its normal left-click behavior (add a folder),
    // while its context menu exposes the other kinds of terminal entry.
    if (sidebarVisible_ && leftBar.contains(x, y) &&
        workspaceAddRect_.contains(x, y)) {
        POINT point{xi, yi};
        ClientToScreen(hwnd_, &point);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 60, L"SSH");
        AppendMenuW(menu, MF_STRING, 61, L"Serial");
        AppendMenuW(menu, MF_STRING, 62, L"Folder");
        const int action = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0,
            hwnd_, nullptr);
        DestroyMenu(menu);
        if (action == 60) addWorkspaceSsh();
        else if (action == 61) addWorkspaceSerial();
        else if (action == 62) addWorkspaceFolder();
        return;
    }

    // Right-click a tab → context menu (acts on that tab).
    if (tabBar.contains(x, y)) {
        for (size_t i = 0; i < tabRects_.size(); ++i) {
            if (!tabRects_[i].contains(x, y)) continue;
            activeTab_ = i;
            clearSelection();
            updateTitle();
            openTabMenu(xi, yi);
            return;
        }
        return;
    }

    if (filesPanelVisible_ && rightPanel.contains(x, y)) {
        openFileMenu(xi, yi);
        return;
    }

    // Right-click inside a terminal pane → the app when it tracks the mouse
    // (mc, vim…; Shift bypasses), else the copy / paste / find menu. There is
    // no WM_RBUTTONUP handler, so send the release right away.
    if (panes.contains(x, y)) {
        if (forwardMouse(0 /*press*/, 2 /*right*/, xi, yi)) {
            forwardMouse(1 /*release*/, 2, xi, yi);
            return;
        }
        openPaneMenu(xi, yi);
        return;
    }

    if (!sidebarVisible_ || !leftBar.contains(x, y)) return;

    for (const SidebarRow& row : sidebarRows_) {
        if (!row.rect.contains(x, y)) continue;
        if (row.kind == RowKind::SshHost) {
            if (row.repo < 0 || row.repo >= static_cast<int>(sshHosts_.size()))
                return;
            POINT point{xi, yi};
            ClientToScreen(hwnd_, &point);
            HMENU sshMenu = CreatePopupMenu();
            AppendMenuW(sshMenu, MF_STRING, 40, L"Connect");
            AppendMenuW(sshMenu, MF_STRING, 41, L"Edit settings…");
            AppendMenuW(sshMenu, MF_STRING, 42, L"Diagnose connection");
            AppendMenuW(sshMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(sshMenu, MF_STRING, 43, L"Remove");
            const int action = TrackPopupMenu(
                sshMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0,
                hwnd_, nullptr);
            DestroyMenu(sshMenu);
            if (action == 41) {
                editWorkspaceSsh(row.repo);
                return;
            }
            if (action == 43) {
                removeWorkspaceSsh(row.repo);
                return;
            }
            if (action == 40) {
                const SshProfile& profile = sshHosts_[row.repo];
                SessionContext context;
                context.role = SessionRole::Ssh;
                context.taskName = profile.name;
                context.sshProfile = profile;
                newTabShell(buildSshCommand(profile), homeDir(), context);
            } else if (action == 42) {
                const std::wstring command =
                    buildSshDiagnosticCommand(sshHosts_[row.repo]);
                if (!command.empty()) newTabShell(command, homeDir());
            }
            return;
        }
        if (row.kind == RowKind::SerialPort) {
            if (row.repo < 0 || row.repo >= static_cast<int>(serialPorts_.size()))
                return;
            POINT point{xi, yi};
            ClientToScreen(hwnd_, &point);
            HMENU serialMenu = CreatePopupMenu();
            AppendMenuW(serialMenu, MF_STRING, 44, L"Connect");
            AppendMenuW(serialMenu, MF_STRING, 45, L"Edit settings…");
            AppendMenuW(serialMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(serialMenu, MF_STRING, 46, L"Remove");
            const int action = TrackPopupMenu(
                serialMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y,
                0, hwnd_, nullptr);
            DestroyMenu(serialMenu);
            if (action == 44) newTabSerial(serialPorts_[row.repo]);
            else if (action == 45) editWorkspaceSerial(row.repo);
            else if (action == 46) removeWorkspaceSerial(row.repo);
            return;
        }
        if (row.kind == RowKind::SshHeader) {
            sshExpanded_ = !sshExpanded_;
            markRenderDirty();
            return;
        }
        if (row.kind == RowKind::SerialHeader) {
            serialExpanded_ = !serialExpanded_;
            markRenderDirty();
            return;
        }
        if (row.kind != RowKind::RepoHeader &&
            row.kind != RowKind::Worktree)
            return;
        auto& repos = workspace_.repos();
        if (row.repo < 0 || row.repo >= static_cast<int>(repos.size())) return;
        Repo& repo = repos[row.repo];

        if (row.worktree < 0) {
            // Project header: folders get folder actions; Git repositories
            // additionally expose worktree, review, and agent workflows.
            POINT pt{ xi, yi };
            ClientToScreen(hwnd_, &pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 4, L"Open");
            AppendMenuW(menu, MF_STRING, 5, L"Open folder in File Explorer");
            if (repo.isGit()) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 1, L"New worktree…");
                if (!agents_.empty()) {
                    HMENU agentMenu = CreatePopupMenu();
                    for (size_t i = 0; i < agents_.size() && i < 50; ++i)
                        AppendMenuW(
                            agentMenu, MF_STRING,
                            static_cast<UINT>(100 + i),
                            agents_[i].name.c_str());
                    AppendMenuW(
                        menu, MF_POPUP,
                        reinterpret_cast<UINT_PTR>(agentMenu),
                        L"New isolated Agent task");
                }
                AppendMenuW(menu, MF_STRING, 6, L"Review changes");
                AppendMenuW(menu, MF_STRING, 7, L"Refresh Git status");
            }
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            const bool favorite = std::any_of(
                favoriteProjects_.begin(), favoriteProjects_.end(),
                [&](const std::wstring& path) {
                    return workspacePathsEqual(path, repo.path);
                });
            AppendMenuW(menu, MF_STRING, 8,
                        favorite ? L"Unpin project" : L"Pin project");
            AppendMenuW(menu, MF_STRING, 2, L"Set icon…");
            AppendMenuW(menu, MF_STRING, 9,
                        row.archived ? L"Restore project" : L"Archive project");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 3, L"Remove from workspace");
            const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                           pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd == 4) {
                rememberRecentProject(repo.path);
                openWorkspaceSession(repo.path, repo.path,
                                     repo.isGit() ? repo.path : L"");
            } else if (cmd == 5) {
                ShellExecuteW(hwnd_, L"open", repo.path.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
            } else if (cmd == 1 && repo.isGit()) {
                bool branchesOk = false;
                const std::wstring branchOutput = runCapture(
                    L"git branch --format=\"%(refname:short)\"",
                    repo.path, &branchesOk);
                std::vector<std::wstring> branches;
                size_t start = 0;
                while (branchesOk && start < branchOutput.size()) {
                    size_t end = branchOutput.find(L'\n', start);
                    if (end == std::wstring::npos) end = branchOutput.size();
                    std::wstring branch = branchOutput.substr(start, end - start);
                    if (!branch.empty() && branch.back() == L'\r')
                        branch.pop_back();
                    if (!branch.empty()) branches.push_back(std::move(branch));
                    start = end + 1;
                }
                const std::wstring previewPrefix =
                    parentDir(repo.path) + L"\\" + repo.name + L"-";
                std::wstring name = inputBoxWithSuggestions(
                    hwnd_, L"New worktree",
                    L"Type a new branch or choose an existing branch:", L"",
                    branches, previewPrefix);
                if (name.empty()) return;
                std::wstring err;
                std::wstring path = workspace_.addWorktree(repo, name, &err);
                if (!path.empty()) {
                    rememberRecentProject(path);
                    openWorkspaceSession(path, repo.path, path);
                }
                else {
                    std::wstring msg = L"git worktree add failed.";
                    if (!err.empty()) msg += L"\n\n" + err;
                    MessageBoxW(hwnd_, msg.c_str(), L"Liney",
                                MB_OK | MB_ICONERROR);
                }
            } else if (repo.isGit() && cmd >= 100 &&
                       cmd < 100 + static_cast<int>(agents_.size())) {
                const size_t agentIndex = static_cast<size_t>(cmd - 100);
                const std::wstring task = inputBox(
                    hwnd_, L"New isolated Agent task",
                    L"Task / branch name (letters, digits, - _ . /):", L"");
                if (task.empty()) return;
                std::wstring err;
                const std::wstring path = workspace_.addWorktree(repo, task, &err);
                if (path.empty()) {
                    std::wstring message = L"Could not create the task worktree.";
                    if (!err.empty()) message += L"\n\n" + err;
                    MessageBoxW(hwnd_, message.c_str(), L"Liney",
                                MB_OK | MB_ICONERROR);
                    return;
                }
                if (TerminalSession* session =
                        newTabShell(agents_[agentIndex].command, path)) {
                    SessionContext context;
                    context.role = SessionRole::Agent;
                    context.projectPath = repo.path;
                    context.worktreePath = path;
                    context.taskName = task;
                    context.agentName = agents_[agentIndex].name;
                    context.testCommand = agents_[agentIndex].testCommand;
                    session->setContext(std::move(context));
                }
            } else if (cmd == 6 && repo.isGit()) {
                if (TerminalSession* session = newTabShell(
                        L"git -C \"" + repo.path + L"\" diff",
                        repo.path)) {
                    SessionContext context;
                    context.projectPath = repo.path;
                    context.worktreePath = repo.path;
                    session->setContext(std::move(context));
                }
            } else if (cmd == 7 && repo.isGit()) {
                repo.loaded = false;
                workspace_.loadWorktrees(repo);
                repo.expanded = true;
                markRenderDirty();
            } else if (cmd == 2) {
                setProjectIcon(repo);
            } else if (cmd == 8) {
                toggleFavoriteProject(repo.path);
            } else if (cmd == 9) {
                toggleProjectArchive(repo);
            } else if (cmd == 3) {
                removeProject(repo);  // erases `repo`; nothing used after
            }
        } else if (row.worktree < static_cast<int>(repo.worktrees.size())) {
            const std::wstring worktreePath = repo.worktrees[row.worktree].path;
            POINT pt{xi, yi};
            ClientToScreen(hwnd_, &pt);
            HMENU worktreeMenu = CreatePopupMenu();
            AppendMenuW(worktreeMenu, MF_STRING, 10, L"Open");
            AppendMenuW(worktreeMenu, MF_STRING, 11, L"Review changes");
            AppendMenuW(worktreeMenu, MF_STRING, 12, L"Refresh Git status");
            AppendMenuW(worktreeMenu, MF_SEPARATOR, 0, nullptr);
            const bool mainWorktree =
                workspacePathsEqual(worktreePath, repo.path);
            AppendMenuW(
                worktreeMenu,
                MF_STRING | (mainWorktree ? MF_GRAYED : 0), 13,
                mainWorktree ? L"Main worktree (cannot remove)"
                             : L"Remove worktree…");
            const int action = TrackPopupMenu(worktreeMenu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
            DestroyMenu(worktreeMenu);
            if (action == 10) {
                rememberRecentProject(worktreePath);
                openWorkspaceSession(worktreePath, repo.path, worktreePath);
            } else if (action == 11) {
                if (TerminalSession* session = newTabShell(
                        L"git -C \"" + worktreePath + L"\" diff",
                        worktreePath)) {
                    SessionContext context;
                    context.projectPath = repo.path;
                    context.worktreePath = worktreePath;
                    session->setContext(std::move(context));
                }
            } else if (action == 12) {
                workspace_.refreshStatus(repo.worktrees[row.worktree]);
                markRenderDirty();
                showToast(L"Git status refreshed");
            } else if (action == 13 && !mainWorktree) {
                std::wstring msg = L"Remove worktree?\n\n" + worktreePath +
                    L"\n\nGit will refuse if it contains uncommitted changes.";
                if (MessageBoxW(hwnd_, msg.c_str(), L"Remove worktree",
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                    return;
                std::wstring err;
                if (!workspace_.removeWorktree(repo, worktreePath, &err)) {
                    std::wstring m = L"git worktree remove failed.";
                    if (!err.empty()) m += L"\n\n" + err;  // e.g. "use --force"
                    MessageBoxW(hwnd_, m.c_str(), L"Liney",
                                MB_OK | MB_ICONERROR);
                }
            }
        }
        return;
    }
}

void Window::onMouseMove(int xi, int yi) {
    lastMouseX_ = xi;
    lastMouseY_ = yi;
    if (panelResize_ != PanelResize::None) {
        updatePanelWidthFromPointer(xi);
        return;
    }
    if (fileScrollDragging_) {
        const float travel = fileScrollTrackRect_.h -
            fileScrollThumbRect_.h;
        if (travel > 0.0f && fileScrollMaxOffset_ > 0.0f) {
            const float thumbY = static_cast<float>(yi) -
                fileScrollDragOffset_;
            const float position = std::clamp(
                (thumbY - fileScrollTrackRect_.y) / travel, 0.0f, 1.0f);
            fileScrollOffset_ = position * fileScrollMaxOffset_;
            markRenderDirty();
        }
        return;
    }
    if (fileDragPending_) {
        const int dragX = std::abs(xi - fileDragStart_.x);
        const int dragY = std::abs(yi - fileDragStart_.y);
        const int threshold = std::max(GetSystemMetrics(SM_CXDRAG),
                                       GetSystemMetrics(SM_CYDRAG));
        if (!fileDragActive_ && std::max(dragX, dragY) >= threshold)
            fileDragActive_ = true;
        if (fileDragActive_) {
            Rect leftBar, rightPanel, tabBar, panes;
            regions(leftBar, rightPanel, tabBar, panes);
            if (!fileDragRemote_ &&
                !rightPanel.contains(static_cast<float>(xi),
                                     static_cast<float>(yi))) {
                const std::wstring path = fileDragPath_;
                fileDragPending_ = false;
                fileDragActive_ = false;
                fileDragPath_.clear();
                fileDragSessionKey_.clear();
                ReleaseCapture();
                startExternalFileDrag(path);
                return;
            }
            if (rightPanel.contains(static_cast<float>(xi),
                                    static_cast<float>(yi))) {
                if (yi < static_cast<int>(rightPanel.y + metrics_.rowH() * 2))
                    fileScrollOffset_ = std::max(
                        0.0f, fileScrollOffset_ - metrics_.rowH());
                else if (yi > static_cast<int>(rightPanel.bottom() -
                                                metrics_.rowH() * 2))
                    fileScrollOffset_ += metrics_.rowH();
            }
            markRenderDirty();
        }
        return;
    }
    // Track which tab the pointer is over so its × button shows (and so the ×
    // hover-highlights). Only meaningful while not dragging.
    if (tabDragIndex_ < 0 && !selecting_ && !dragDivider_) {
        const float x = static_cast<float>(xi), y = static_cast<float>(yi);
        int hover = -1;
        for (size_t i = 0; i < tabRects_.size(); ++i)
            if (tabRects_[i].contains(x, y)) { hover = static_cast<int>(i); break; }
        if (hover != hoverTab_) { hoverTab_ = hover; markRenderDirty(); }
    }
    if (tabDragIndex_ >= 0) {
        const float x = static_cast<float>(xi), y = static_cast<float>(yi);
        for (size_t i = 0; i < tabRects_.size(); ++i) {
            if (static_cast<int>(i) == tabDragIndex_) continue;
            if (!tabRects_[i].contains(x, y)) continue;
            // Move the dragged tab to position i, keeping it active.
            auto moved = std::move(tabs_[tabDragIndex_]);
            tabs_.erase(tabs_.begin() + tabDragIndex_);
            tabs_.insert(tabs_.begin() + i, std::move(moved));
            tabDragIndex_ = static_cast<int>(i);
            activeTab_ = i;
            break;
        }
        return;
    }
    if (dragDivider_) {
        Pane* s = dragDivider_;
        const float x = static_cast<float>(xi), y = static_cast<float>(yi);
        float r = s->ratio;
        if (s->dir == SplitDir::Cols && s->rect.w > 1.0f)
            r = (x - s->rect.x) / s->rect.w;
        else if (s->dir == SplitDir::Rows && s->rect.h > 1.0f)
            r = (y - s->rect.y) / s->rect.h;
        s->ratio = r < 0.05f ? 0.05f : (r > 0.95f ? 0.95f : r);
        return;
    }
    if (selecting_ && selPane_ && selPane_->session) {
        // Auto-scroll when the drag runs past the top/bottom edge of the pane,
        // so a selection can extend into scrollback (or back toward live
        // output); the anchor is buffer-tracked so it stays put.
        const Rect& pr = selPane_->rect;
        if (yi < static_cast<int>(pr.y)) scrollActive(1);
        else if (yi > static_cast<int>(pr.bottom())) scrollActive(-1);
        int cx = 0, cy = 0;
        if (!paneCellAt(selPane_, xi, yi, cx, cy)) return;
        if (!selDragged_ && cx == selDragCX_ && cy == selDragCY_)
            return;  // still inside the press cell: not a drag yet
        selDragged_ = true;
        selPane_->session->selectionDragTo(cx, cy);
        return;
    }
    // Not a local gesture: motion goes to mouse-tracking apps (the encoder
    // only emits what the app's tracking mode asked for, deduped per cell).
    forwardMouse(2 /*motion*/, (mouseButtonsDown_ & (1 << 1)) ? 1 : 0, xi, yi);
}

void Window::onMouseUp(int xi, int yi) {
    if (panelResize_ != PanelResize::None) {
        panelResize_ = PanelResize::None;
        savePanelLayout();
        ReleaseCapture();
        markRenderDirty();
        return;
    }
    if (fileScrollDragging_) {
        fileScrollDragging_ = false;
        fileScrollDragOffset_ = 0.0f;
        ReleaseCapture();
        markRenderDirty();
        return;
    }
    if (fileDragPending_) {
        finishFileDrag(xi, yi);
        return;
    }
    if (tabDragIndex_ >= 0) {
        tabDragIndex_ = -1;
        ReleaseCapture();
        return;
    }
    if (dragDivider_) {
        dragDivider_ = nullptr;
        ReleaseCapture();
        return;
    }
    if (selecting_) {
        selecting_ = false;
        ReleaseCapture();
        maybeCopyOnSelect();  // PuTTY-style copy-on-select (when enabled)
        return;
    }
    // Close out a press that was forwarded to a mouse-tracking app. If the
    // forward is refused (Shift now held / tracking turned off), still drop
    // the button bit so it can't wedge.
    if (mouseButtonsDown_ & (1 << 1)) {
        if (!forwardMouse(1 /*release*/, 1, xi, yi))
            mouseButtonsDown_ &= ~(1 << 1);
    }
}

void Window::updatePanelWidthFromPointer(int xi) {
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) return;
    const float windowWidth = static_cast<float>(client.right - client.left);
    const float scale = std::max(0.01f, metrics_.uiScale);
    const float minWidth = 144.0f;
    const float maxWidth = 640.0f;
    const float physicalWidth = panelResize_ == PanelResize::Sidebar
        ? static_cast<float>(xi)
        : windowWidth - static_cast<float>(xi);
    const float logicalWidth = physicalWidth / scale;
    const float* target = nullptr;
    if (panelResize_ == PanelResize::Sidebar) target = &sidebarWidth_;
    else if (panelResize_ == PanelResize::Files) target = &filesPanelWidth_;
    if (!target) return;
    const float clamped = std::clamp(logicalWidth, minWidth, maxWidth);
    if (*target == clamped) return;
    if (panelResize_ == PanelResize::Sidebar) sidebarWidth_ = clamped;
    else filesPanelWidth_ = clamped;
    markRenderDirty();
}

bool Window::paneCellAt(const Pane* p, int px, int py, int& cx, int& cy) const {
    if (!p || !p->session) return false;
    const Grid& g = p->session->grid();
    if (g.cols < 1 || g.rows < 1) return false;
    const float pad = metrics_.panePad();
    int x = static_cast<int>((px - p->rect.x - pad) / metrics_.cellW);
    int y = static_cast<int>((py - p->rect.y - pad) / metrics_.cellH);
    cx = x < 0 ? 0 : (x >= g.cols ? g.cols - 1 : x);
    cy = y < 0 ? 0 : (y >= g.rows ? g.rows - 1 : y);
    return true;
}

void Window::clearSelection() {
    if ((fileDragPending_ || fileScrollDragging_) && GetCapture() == hwnd_)
        ReleaseCapture();
    selecting_ = false;
    if (selPane_ && selPane_->session) selPane_->session->selectionClear();
    selPane_ = nullptr;
    dragDivider_ = nullptr;
    tabDragIndex_ = -1;
    fileDragPending_ = false;
    fileDragActive_ = false;
    fileDragPath_.clear();
    fileDragSessionKey_.clear();
    fileScrollDragging_ = false;
    fileScrollDragOffset_ = 0.0f;
}

bool Window::paneHasSelection() const {
    return selPane_ && selPane_->session && selPane_->session->hasSelection();
}

std::wstring Window::selectionText() const {
    if (!selPane_ || !selPane_->session) return L"";
    return utf8ToWide(selPane_->session->selectionUtf8());
}

void Window::copySelection() {
    const std::wstring text = selectionText();
    if (text.empty()) return;
    // The core emits LF line breaks; the Windows clipboard wants CRLF.
    std::wstring crlf;
    crlf.reserve(text.size() + 16);
    for (wchar_t c : text) {
        if (c == L'\n') crlf += L"\r\n";
        else crlf.push_back(c);
    }
    setClipboardText(crlf);
}

void Window::setClipboardText(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    bool copied = false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            copied = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    if (copied) showToast(L"Copied to clipboard");
}

void Window::paste() {
    auto* s = activeSession();
    if (!s || !OpenClipboard(hwnd_)) return;
    std::wstring text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            text = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (text.empty()) return;

    // Normalize CRLF / LF to CR (what shells expect from "Enter").
    std::wstring norm;
    norm.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'\r') {
            norm.push_back(L'\r');
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (c == L'\n') {
            norm.push_back(L'\r');
        } else {
            norm.push_back(c);
        }
    }

    // Multi-line pastes execute every embedded newline as "Enter"; confirm
    // first so a stray copy can't run commands (config: multiLinePasteWarning).
    if (multiLinePasteWarning_ && norm.find(L'\r') != std::wstring::npos) {
        int newlines = 0;
        for (wchar_t c : norm) if (c == L'\r') ++newlines;
        const std::wstring msg =
            L"The clipboard contains " + std::to_wstring(newlines + 1) +
            L" lines; each line break runs as Enter.\n\nPaste anyway?";
        if (MessageBoxW(hwnd_, msg.c_str(), L"Liney — paste",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }

    int bytes = WideCharToMultiByte(CP_UTF8, 0, norm.data(),
                                    static_cast<int>(norm.size()), nullptr, 0,
                                    nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, norm.data(), static_cast<int>(norm.size()),
                        utf8.data(), bytes, nullptr, nullptr);

    s->scrollToBottom();
    if (s->serialRawTextMode()) {
        s->appendSerialText(norm.c_str(), norm.size());
        return;
    }
    if (s->bracketedPaste()) {
        const std::string out = "\x1b[200~" + utf8 + "\x1b[201~";
        s->sendBytes(out.data(), out.size());
    } else {
        s->sendBytes(utf8.data(), utf8.size());
    }
}

void Window::selectWordAt(Pane* p, int cx, int cy) {
    if (!p || !p->session) return;
    if (selPane_ && selPane_ != p && selPane_->session)
        selPane_->session->selectionClear();
    selPane_ = p;
    p->session->selectionWord(cx, cy);  // core word rules (CJK-aware)
}

void Window::selectLineAt(Pane* p, int cy) {
    if (!p || !p->session) return;
    if (selPane_ && selPane_ != p && selPane_->session)
        selPane_->session->selectionClear();
    selPane_ = p;
    p->session->selectionLine(0, cy);
}

void Window::selectAllActive() {
    Tab* t = activeTab();
    if (!t || !t->active() || !t->active()->session) return;
    if (selPane_ && selPane_ != t->active() && selPane_->session)
        selPane_->session->selectionClear();
    selPane_ = t->active();
    selPane_->session->selectionAll();  // whole buffer, scrollback included
}

void Window::maybeCopyOnSelect() {
    if (copyOnSelect_ && paneHasSelection()) copySelection();  // keep highlight
}

bool Window::forwardMouse(int action, int button, int xi, int yi) {
    if (keyDown(VK_SHIFT)) return false;  // Shift bypasses to local selection
    Tab* t = activeTab();
    if (!t) return false;
    Pane* leaf = t->hitTest(static_cast<float>(xi), static_cast<float>(yi));
    if (!leaf || !leaf->session) return false;
    TerminalSession* s = leaf->session.get();
    if (!s->mouseTracking()) {
        mouseButtonsDown_ = 0;
        return false;
    }
    const Rect& pr = leaf->rect;
    const float pad = metrics_.panePad();
    const std::string seq = s->encodeMouse(
        action, button, static_cast<float>(xi) - pr.x - pad,
        static_cast<float>(yi) - pr.y - pad, false, keyDown(VK_CONTROL),
        keyDown(VK_MENU), mouseButtonsDown_ != 0,
        static_cast<unsigned>(metrics_.cellW),
        static_cast<unsigned>(metrics_.cellH),
        static_cast<unsigned>(pr.w - pad * 2.0f),
        static_cast<unsigned>(pr.h - pad * 2.0f));
    if (action == 0 && button >= 1 && button <= 3)
        mouseButtonsDown_ |= 1 << button;
    else if (action == 1)
        mouseButtonsDown_ &= ~(1 << button);
    if (!seq.empty()) s->sendBytes(seq.data(), seq.size());
    return true;
}

bool Window::updateCursor() {
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    ScreenToClient(hwnd_, &pt);
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    Rect leftBar, rightPanel, tabBar, panes;
    regions(leftBar, rightPanel, tabBar, panes);

    if (sidebarResizeRect_.contains(x, y) || filesResizeRect_.contains(x, y) ||
        panelResize_ != PanelResize::None) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return true;
    }

    if (panes.contains(x, y)) {
        if (paneCloseRect_.contains(x, y)) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return true;
        }
        if (Tab* t = activeTab()) {
            if (Pane* d = t->splitDividerAt(x, y, 4.0f)) {
                SetCursor(LoadCursorW(nullptr,
                    d->dir == SplitDir::Cols ? IDC_SIZEWE : IDC_SIZENS));
                return true;
            }
        }
        if (findActive_ && findBarRect_.contains(x, y)) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return true;
        }
        if (Tab* t = activeTab()) {
            if (Pane* leaf = t->hitTest(x, y); leaf && leaf->session) {
                int cx = 0, cy = 0;
                if (paneCellAt(leaf, static_cast<int>(x),
                               static_cast<int>(y), cx, cy)) {
                    std::vector<TerminalUrlHit> urlHits;
                    if (!leaf->session->hyperlinkAt(cx, cy).empty() ||
                        terminalUrlAt(leaf->session->grid(), cy, cx,
                                      urlHits)) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return true;
                    }
                }
            }
        }
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));  // over terminal text
        return true;
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));      // chrome
    return true;
}

void Window::openTerminalUrlMenu(const std::wstring& url, int xi, int yi) {
    if (url.empty()) return;
    POINT pt{xi, yi};
    ClientToScreen(hwnd_, &pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"Copy");
    AppendMenuW(menu, MF_STRING | (isOpenableTerminalUrl(url) ? 0 : MF_GRAYED),
                2, L"Open");
    const int action = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (action == 1) {
        setClipboardText(url);
    } else if (action == 2 && isOpenableTerminalUrl(url)) {
        if (!openTerminalUrl(hwnd_, url)) showToast(L"Could not open link", true);
    }
}

void Window::openPaneMenu(int xi, int yi) {
    // Focus the pane under the cursor so copy/paste target it.
    if (Tab* t = activeTab())
        if (Pane* leaf = t->hitTest(static_cast<float>(xi), static_cast<float>(yi)))
            t->setActive(leaf);

    POINT pt{ xi, yi };
    ClientToScreen(hwnd_, &pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (paneHasSelection() ? 0 : MF_GRAYED), 1,
                L"Copy\tCtrl+Shift+C");
    AppendMenuW(m, MF_STRING, 2, L"Paste\tShift+Insert");
    AppendMenuW(m, MF_STRING, 3, L"Select all\tCtrl+Shift+A");
    AppendMenuW(m, MF_STRING, 4, L"Find in terminal…\tCtrl+F");
    TerminalSession* menuSession = activeSession();
    const bool serialSession = menuSession && menuSession->isSerial();
    if (serialSession) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (menuSession->exited() ? MF_GRAYED : 0),
                    42, L"Send hex bytes…");
    }
    if (menuSession && menuSession->exited()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 41, L"Restart shell in this directory");
    }
    const bool agentSession = menuSession &&
        menuSession->context().role == SessionRole::Agent;
    if (agentSession) {
        HMENU agent = CreatePopupMenu();
        AppendMenuW(agent, MF_STRING, 20, L"Review changes");
        if (!menuSession->context().testCommand.empty())
            AppendMenuW(agent, MF_STRING, 21, L"Run project verification");
        AppendMenuW(agent, MF_STRING, 28, L"Open task folder in…");
        if (menuSession->exited() &&
            !menuSession->context().worktreePath.empty()) {
            AppendMenuW(agent, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(agent, MF_STRING, 29, L"Safely remove task worktree…");
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(agent), L"Agent");
    }
    const CommandBlock* lastCommand = nullptr;
    if (menuSession && !menuSession->commandBlocks().empty())
        lastCommand = &menuSession->commandBlocks().back();
    if (lastCommand) {
        HMENU command = CreatePopupMenu();
        const wchar_t* state = lastCommand->state == CommandState::Running ? L"running" :
                               lastCommand->state == CommandState::Succeeded ? L"succeeded" :
                                                                               L"failed";
        const std::wstring summary = L"Last command: " + std::wstring(state) +
            L" (" + std::to_wstring(lastCommand->duration.count()) + L" ms)";
        AppendMenuW(command, MF_STRING | MF_DISABLED, 0, summary.c_str());
        AppendMenuW(command,
                    MF_STRING |
                        (lastCommand->command.empty() ? MF_GRAYED : 0),
                    22, L"Copy command");
        AppendMenuW(command,
                    MF_STRING |
                        (lastCommand->command.empty() ||
                                 lastCommand->state == CommandState::Running
                             ? MF_GRAYED
                             : 0),
                    23, L"Run again");
        AppendMenuW(command, MF_STRING, 24, L"Jump to previous command");
        AppendMenuW(command, MF_STRING, 25, L"Jump to next command");
        AppendMenuW(command, MF_STRING, 26, L"Copy output");
        AppendMenuW(command, MF_STRING, 27,
                    lastCommand->bookmarked ? L"Remove command bookmark"
                                            : L"Bookmark last command");
        AppendMenuW(command,
                    MF_STRING |
                        (aiProvider_ == L"off" || aiBusy_ ||
                                 lastCommand->state == CommandState::Running
                             ? MF_GRAYED
                             : 0),
                    40, lastCommand->state == CommandState::Failed
                            ? L"Diagnose failure with AI…"
                            : L"Explain with AI…");
        if (!agentSession) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(command),
                    L"Last command");
    }

    // Keep the root context menu short. Layout operations remain discoverable
    // under one stable submenu and context-only no-ops are omitted.
    const bool split = activeTab() && activeTab()->isSplit();
    HMENU layout = CreatePopupMenu();
    AppendMenuW(layout, MF_STRING, 7, L"Split right\tAlt+D");
    AppendMenuW(layout, MF_STRING, 8, L"Split down\tShift+Alt+D");
    if (split) {
        AppendMenuW(layout, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(layout, MF_STRING, 9, L"Zoom or restore pane\tCtrl+Shift+Z");
        AppendMenuW(layout, MF_STRING, 10, L"Equalize panes\tCtrl+Shift+E");
        AppendMenuW(layout, MF_STRING, 11, L"Swap with next pane");
        AppendMenuW(layout, MF_STRING, 12, L"Move pane to new tab");
        AppendMenuW(layout, MF_STRING, 13, L"Move pane forward");
        AppendMenuW(layout, MF_STRING, 6, L"Close other panes");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(layout),
                L"Pane layout");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5,
                split ? L"Close pane\tCtrl+Shift+W"
                      : L"Close tab\tCtrl+Shift+W");
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y,
                                   0, hwnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case 1: copySelection(); clearSelection(); break;
    case 2: paste(); break;
    case 3: selectAllActive(); break;
    case 4: openFind(); break;
    case 42:
        if (menuSession && menuSession->isSerial()) {
            const std::wstring input = inputBox(
                hwnd_, L"Send serial bytes",
                L"Hex bytes (for example: 7e 00 ff):", L"");
            if (!input.empty()) {
                std::wstring error;
                if (!menuSession->sendSerialHexInput(input, &error))
                    MessageBoxW(hwnd_, error.c_str(), L"Liney - serial input",
                                MB_OK | MB_ICONWARNING);
            }
        }
        break;
    case 5: closeActivePaneConfirming(); break;
    case 6: closeOtherPanes(); break;
    case 7: splitActive(SplitDir::Cols); break;
    case 8: splitActive(SplitDir::Rows); break;
    case 9: toggleZoom(); break;
    case 10: equalizePanes(); break;
    case 11: executePaletteAction(21); break;
    case 12: executePaletteAction(22); break;
    case 13: executePaletteAction(23); break;
    case 20:
        if (menuSession) {
            const std::wstring cwd = !menuSession->context().worktreePath.empty()
                                         ? menuSession->context().worktreePath
                                         : menuSession->cwd();
            if (!cwd.empty()) newTabShell(L"git -C \"" + cwd + L"\" diff", cwd);
        }
        break;
    case 21:
        if (menuSession && !menuSession->context().testCommand.empty()) {
            const std::wstring cwd = !menuSession->context().worktreePath.empty()
                                         ? menuSession->context().worktreePath
                                         : menuSession->cwd();
            const SessionContext source = menuSession->context();
            if (TerminalSession* verification =
                    newTabShell(source.testCommand, cwd)) {
                SessionContext context = source;
                context.taskName += L" verification";
                verification->setContext(std::move(context));
            }
        }
        break;
    case 28:
        openDirectoryMenu();
        break;
    case 29:
        if (menuSession && menuSession->exited() &&
            !menuSession->context().worktreePath.empty()) {
            const std::wstring path = menuSession->context().worktreePath;
            if (MessageBoxW(hwnd_,
                    (L"Remove the completed task worktree?\n\n" + path +
                     L"\n\nUncommitted changes are never forced away.").c_str(),
                    L"Liney", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                break;
            bool found = false;
            std::wstring error;
            for (Repo& repo : workspace_.repos()) {
                for (const Worktree& wt : repo.worktrees) {
                    if (_wcsicmp(wt.path.c_str(), path.c_str()) == 0) {
                        found = workspace_.removeWorktree(repo, path, &error);
                        break;
                    }
                }
                if (found || !error.empty()) break;
            }
            if (!found) {
                std::wstring message = L"The worktree was not removed.";
                if (!error.empty()) message += L"\n\n" + error;
                MessageBoxW(hwnd_, message.c_str(), L"Liney",
                            MB_OK | MB_ICONERROR);
            }
        }
        break;
    case 22:
        if (lastCommand) setClipboardText(lastCommand->command);
        break;
    case 23:
        if (lastCommand && !lastCommand->command.empty()) {
            const std::wstring line = lastCommand->command + L"\r";
            sendUtf16(line.c_str(), line.size());
        }
        break;
    case 24: if (menuSession) menuSession->jumpPreviousCommand(); break;
    case 25: if (menuSession) menuSession->jumpNextCommand(); break;
    case 26:
        if (menuSession && !menuSession->commandBlocks().empty())
            setClipboardText(utf8ToWide(menuSession->commandOutputUtf8(
                menuSession->commandBlocks().size() - 1)));
        break;
    case 27:
        if (menuSession) menuSession->toggleBookmarkLastCommand();
        break;
    case 40:
        requestAiForLastCommand(menuSession);
        break;
    case 41:
        restartSession(menuSession);
        break;
    default: break;
    }
}

} // namespace liney
