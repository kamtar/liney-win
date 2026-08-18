#pragma once

#include <windows.h>

#include "core/SerialProfiles.h"
#include "core/SshProfiles.h"
#include "core/Themes.h"

namespace liney {

enum class ConnectionDialogKind { Ssh, Serial };

// The dialog edits one of these profiles depending on `kind`. Keeping the
// result object shared makes the SSH and serial entry points use the same
// visual and interaction model without coupling their transport code.
struct ConnectionDialogValues {
    SshProfile ssh;
    SerialProfile serial;
};

// Returns true when the user presses Add/Connect and the selected profile is
// valid. The caller owns persistence and opening the resulting session.
bool showConnectionDialog(HWND owner, ConnectionDialogKind kind,
                          ConnectionDialogValues& values,
                          const UiTheme& uiTheme);

} // namespace liney
