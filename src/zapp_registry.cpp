#include "zapp_registry.h"
#include <SD.h>
#include "config.h"

static bool ensureAppsDir() {
    if (!SD.exists(ZAPP_DIR)) return SD.mkdir(ZAPP_DIR);
    return true;
}

// Registry format: one line per app, "name|blockedFlag" (0/1). Kept
// deliberately dumber than settings.cpp's /zyro.conf (no key=value parsing
// needed) since there's only ever these two fields.
static std::vector<ZappEntry> readRegistry() {
    std::vector<ZappEntry> out;
    File f = SD.open(ZAPP_REGISTRY_PATH, FILE_READ);
    if (!f) return out;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        int bar = line.lastIndexOf('|');
        if (bar < 0) continue;
        ZappEntry e;
        e.name = line.substring(0, bar);
        e.blocked = line.substring(bar + 1).toInt() != 0;
        if (e.name.length() > 0) out.push_back(e);
    }
    f.close();
    return out;
}

static bool writeRegistry(const std::vector<ZappEntry> &entries) {
    if (!ensureAppsDir()) return false;
    File f = SD.open(ZAPP_REGISTRY_PATH, FILE_WRITE);
    if (!f) return false;
    // FILE_WRITE on this SD library appends rather than truncates on some
    // cores - explicitly seek to 0 and let short files just end early isn't
    // safe (stale tail bytes), so remove-then-recreate instead.
    f.close();
    SD.remove(ZAPP_REGISTRY_PATH);
    f = SD.open(ZAPP_REGISTRY_PATH, FILE_WRITE);
    if (!f) return false;
    for (auto &e : entries) {
        f.print(e.name);
        f.print('|');
        f.println(e.blocked ? 1 : 0);
    }
    f.close();
    return true;
}

std::vector<ZappEntry> zappRegistryList() {
    return readRegistry();
}

static bool validAppName(const String &name) {
    if (name.length() == 0 || name.length() > ZAPP_NAME_MAX) return false;
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        bool ok = isalnum((unsigned char)c) || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool zappRegistryInstall(const String &srcPathOnSd, String *outName) {
    if (!ensureAppsDir()) return false;

    int slash = srcPathOnSd.lastIndexOf('/');
    String fileName = (slash >= 0) ? srcPathOnSd.substring(slash + 1) : srcPathOnSd;
    int dot = fileName.lastIndexOf('.');
    String name = (dot > 0) ? fileName.substring(0, dot) : fileName;
    name.trim();

    if (!validAppName(name)) return false;
    if (!srcPathOnSd.endsWith(".zApp") && !srcPathOnSd.endsWith(".ZAPP") && !srcPathOnSd.endsWith(".zapp")) return false;

    String appDir  = String(ZAPP_DIR) + "/" + name;
    String dataDir = appDir + "/data";
    String dstPath = appDir + "/" + name + ".zApp";

    if (!SD.exists(appDir) && !SD.mkdir(appDir)) return false;
    if (!SD.exists(dataDir) && !SD.mkdir(dataDir)) return false;

    File src = SD.open(srcPathOnSd, FILE_READ);
    if (!src) return false;
    SD.remove(dstPath);
    File dst = SD.open(dstPath, FILE_WRITE);
    if (!dst) { src.close(); return false; }

    uint8_t buf[512];
    while (true) {
        int n = src.read(buf, sizeof(buf));
        if (n <= 0) break;
        dst.write(buf, n);
    }
    src.close();
    dst.close();

    auto entries = readRegistry();
    bool found = false;
    for (auto &e : entries) {
        if (e.name == name) { e.blocked = false; found = true; break; }
    }
    if (!found) entries.push_back({ name, false });
    if (!writeRegistry(entries)) return false;

    if (outName) *outName = name;
    return true;
}

void zappRegistrySetBlocked(const String &name, bool blocked) {
    auto entries = readRegistry();
    for (auto &e : entries) {
        if (e.name == name) { e.blocked = blocked; break; }
    }
    writeRegistry(entries);
}

static void removeDirRecursive(const String &path) {
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); SD.remove(path); return; }
    File entry = dir.openNextFile();
    while (entry) {
        String childPath = path + "/" + String(entry.name());
        bool isDir = entry.isDirectory();
        entry.close();
        if (isDir) removeDirRecursive(childPath);
        else SD.remove(childPath);
        entry = dir.openNextFile();
    }
    dir.close();
    SD.rmdir(path);
}

bool zappRegistryRemove(const String &name) {
    if (!validAppName(name)) return false;
    removeDirRecursive(String(ZAPP_DIR) + "/" + name);

    auto entries = readRegistry();
    std::vector<ZappEntry> kept;
    for (auto &e : entries) if (e.name != name) kept.push_back(e);
    return writeRegistry(kept);
}

String zappSandboxResolve(const String &appName, const char *relPath) {
    if (!validAppName(appName)) return "";
    if (!relPath) return "";
    String rel(relPath);
    if (rel.length() == 0) return "";
    if (rel.startsWith("/")) return "";
    if (rel.indexOf("..") >= 0) return "";
    return String(ZAPP_DIR) + "/" + appName + "/data/" + rel;
}
