#pragma once
#include <Arduino.h>
#include <vector>

// ============================================================================
// Tracks installed .zApp apps under /apps on the SD card. Plain text, one
// line per app (same "no ArduinoJson dependency" philosophy as
// settings.cpp's /zyro.conf) - editable by hand on a PC if needed.
//
// Layout on the card, per installed app "Foo":
//   /apps/registry.txt          <- one line per app: "Foo|1706000000|0"
//                                   name|installedUnixOrMillis|blockedFlag
//   /apps/Foo/Foo.zApp           <- the binary, copied in at install time
//   /apps/Foo/data/               <- ONLY folder the app's ZyroFile API can touch
//
// blockedFlag gets set by zapp_loader.cpp's crash guard the moment an app is
// caught having reset the device mid-run - it still shows up in "My Apps"
// but flagged, and the user has to explicitly re-confirm launching it, so a
// bad app can't silently reboot-loop the device forever.
// ============================================================================

struct ZappEntry {
    String name;
    bool blocked;
};

// Loads/parses /apps/registry.txt fresh from SD every call - registry is tiny
// and this only happens when the My Apps screen is opened/refreshed, not on
// every frame.
std::vector<ZappEntry> zappRegistryList();

// Copies srcPathOnSd (any existing .zApp file, e.g. selected via the SD file
// browser) into /apps/<name>/<name>.zApp, creates /apps/<name>/data/, and
// adds/updates its registry.txt line. name is derived from the source
// filename with the extension stripped. Returns false on any I/O failure or
// if name is empty/invalid.
bool zappRegistryInstall(const String &srcPathOnSd, String *outName);

// Marks/clears the blocked flag for one app (used by the crash guard, and by
// the My Apps screen's "un-block" action after the user confirms a retry).
void zappRegistrySetBlocked(const String &name, bool blocked);

// Deletes an app's whole /apps/<name>/ folder and its registry line.
bool zappRegistryRemove(const String &name);

// Resolves a relative path an app hands to ZyroFile into an absolute SD
// path confined inside /apps/<currentAppName>/data/. Rejects ".." and a
// leading "/" outright (returns ""), so a .zApp can never read/write
// anywhere outside its own sandbox folder, no matter what path it asks for.
String zappSandboxResolve(const String &appName, const char *relPath);
