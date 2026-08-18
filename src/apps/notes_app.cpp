#include "app_api.h"
#include <SD.h>
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"
#include "input.h"
#include "menu.h"

// Simple on-device notes editor, backed by plain .txt files under /notes on
// the SD card. Full alphabet entry (not just digits/punctuation) needs
// inputSetTextEntryMode(true) - see input.cpp's mapKeyToNav() - which is
// only available to a firmware app like this one, not a sandboxed .zApp
// (ZyroApi has no such call). That's the whole reason this lives here
// instead of as an example in the SDK.
//
// Editing convention: Escape and Backspace both arrive as InputEvent::BACK
// (same physical ambiguity noted in input.h), but InputResult::ch still
// tells them apart (0x1B vs 0x08/0x7F) - unlike the Wi-Fi/LoRa text fields
// elsewhere in this firmware, which don't use that distinction and so make
// you backspace through everything you typed just to leave. Here: Backspace
// erases a character, Escape saves and closes immediately, regardless of
// how much text is in the note.

namespace NotesApp {

#define NOTES_DIR "/notes"

enum Mode { MODE_LIST = 0, MODE_EDIT };

static Mode currentMode = MODE_LIST;
static Menu *listMenu = nullptr;
static bool exitApp = false;

static std::vector<String> listedFiles; // .txt filenames under /notes, referenced by the list Menu's lambdas

static String currentFile = "";
static String buf = "";
static bool dirty = false;
static uint32_t lastAutosaveMs = 0;
static const uint32_t AUTOSAVE_INTERVAL_MS = 4000;
static const int CHAR_W = 6; // built-in font assumption, matches this firmware's other multi-line screens

static void drawEdit();
static void rebuildListMenu();

static String notePath(const String &name) { return String(NOTES_DIR) + "/" + name; }

static void ensureNotesDir() {
    if (!SD.exists(NOTES_DIR)) SD.mkdir(NOTES_DIR);
}

static void refreshFileList() {
    listedFiles.clear();
    ensureNotesDir();
    File dir = SD.open(NOTES_DIR);
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String n = String(entry.name());
            if (n.endsWith(".txt")) listedFiles.push_back(n);
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

static void saveCurrentNote() {
    if (currentFile.length() == 0) return;
    ensureNotesDir();
    // Same "FILE_WRITE may append rather than truncate on this SD core"
    // gotcha as zapp_registry.cpp's writeRegistry() - remove first to
    // guarantee a clean overwrite instead of stale trailing bytes.
    SD.remove(notePath(currentFile));
    File f = SD.open(notePath(currentFile), FILE_WRITE);
    if (!f) return;
    f.print(buf);
    f.close();
    dirty = false;
}

static void loadNote(const String &name) {
    buf = "";
    File f = SD.open(notePath(name), FILE_READ);
    if (!f) return;
    while (f.available()) buf += (char)f.read();
    f.close();
}

static void enterEdit(const String &name, bool isNew) {
    currentFile = name;
    if (isNew) buf = ""; else loadNote(name);
    dirty = false;
    lastAutosaveMs = millis();
    currentMode = MODE_EDIT;
    inputSetTextEntryMode(true);
    drawEdit();
}

static void startNewNote() {
    ensureNotesDir();
    int n = 1;
    String name;
    do {
        name = "Note" + String(n) + ".txt";
        n++;
    } while (SD.exists(notePath(name)) && n < 1000);
    enterEdit(name, true);
}

static void openNote(const String &name) {
    enterEdit(name, false);
}

static void deleteSelected() {
    if (!listMenu) return;
    int sel = listMenu->selected(); // row 0 is always "+ New Note"
    int fileIdx = sel - 1;
    if (fileIdx < 0 || fileIdx >= (int)listedFiles.size()) return;
    SD.remove(notePath(listedFiles[fileIdx]));
    audioClickBack();
    rebuildListMenu();
}

static void rebuildListMenu() {
    refreshFileList(); // populates listedFiles fully; not touched again until the next rebuild, so the .c_str()/index captures below stay valid
    if (listMenu) { delete listMenu; listMenu = nullptr; }

    std::vector<MenuItem> items;
    items.push_back({ "+ New Note", "+", [](){ startNewNote(); } });
    for (size_t i = 0; i < listedFiles.size(); i++) {
        items.push_back({ listedFiles[i].c_str(), "", [i](){ openNote(listedFiles[i]); } });
    }

    listMenu = new Menu("Notes  (D=delete)", items);
    listMenu->draw();
}

// Wraps text into on-screen lines, one word-wrapped paragraph per '\n' in
// the source (so blank lines the user typed stay blank) - same general idea
// as lora_app.cpp's drawChat(), just wrapping one continuous buffer instead
// of a list of discrete messages.
static std::vector<String> wrapText(const String &text, int maxChars) {
    std::vector<String> lines;
    if (maxChars < 1) maxChars = 1;
    int start = 0;
    while (true) {
        int nl = text.indexOf('\n', start);
        String para = (nl < 0) ? text.substring(start) : text.substring(start, nl);
        int i = 0;
        while (true) {
            if ((int)para.length() - i <= maxChars) {
                lines.push_back(para.substring(i));
                break;
            }
            int brk = para.lastIndexOf(' ', i + maxChars);
            if (brk <= i) brk = i + maxChars; // no space to break on - hard cut
            lines.push_back(para.substring(i, brk));
            i = brk;
            while (i < (int)para.length() && para[i] == ' ') i++;
        }
        if (nl < 0) break;
        start = nl + 1;
    }
    return lines;
}

static void drawEdit() {
    const Theme &t = gSettings.theme();
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);
    gfx->setTextSize(1);
    gfx->setTextColor(t.accent);
    gfx->setCursor(6, TOPBAR_HEIGHT + 2);
    gfx->print(currentFile + (dirty ? " *" : ""));

    int maxChars = (SCREEN_W - 12) / CHAR_W;
    std::vector<String> lines = wrapText(buf, maxChars);

    int footerY = SCREEN_H - 14;
    int top = TOPBAR_HEIGHT + 16;
    int lineH = 14;
    int visibleRows = (footerY - top) / lineH;

    int start = (int)lines.size() > visibleRows ? (int)lines.size() - visibleRows : 0;
    gfx->setTextColor(t.fg);
    for (int i = start; i < (int)lines.size(); i++) {
        int row = i - start;
        gfx->setCursor(6, top + row * lineH);
        gfx->print(lines[i]);
    }

    gfx->drawFastHLine(0, footerY - 2, SCREEN_W, t.dim);
    gfx->setTextColor(t.dim);
    gfx->setCursor(6, footerY + 2);
    gfx->print("ESC=save&close  Backspace=erase  Enter=newline");
}

static void init() {
    exitApp = false;
    currentMode = MODE_LIST;
    rebuildListMenu();
}

static void tick() {
    if (currentMode == MODE_LIST) {
        if (listMenu) listMenu->tick();
        return;
    }
    if (dirty && millis() - lastAutosaveMs > AUTOSAVE_INTERVAL_MS) {
        lastAutosaveMs = millis();
        saveCurrentNote();
        drawEdit(); // clears the "*" dirty marker
    }
}

static void handleInput(const InputResult &in) {
    if (currentMode == MODE_LIST) {
        if (in.type == InputEvent::BACK) {
            exitApp = true;
            audioClickBack();
            return;
        }
        if (in.type == InputEvent::CHAR && (in.ch == 'd' || in.ch == 'D')) {
            deleteSelected();
            return;
        }
        if (listMenu) listMenu->handleInput(in);
        return;
    }

    // MODE_EDIT
    if (in.type == InputEvent::CHAR) {
        buf += in.ch;
        dirty = true;
        drawEdit();
        return;
    }
    if (in.type == InputEvent::OK) {
        buf += '\n';
        dirty = true;
        drawEdit();
        return;
    }
    if (in.type == InputEvent::BACK) {
        if (in.ch == 0x1B) {
            // Escape: save and close right away, whatever's in the buffer.
            saveCurrentNote();
            inputSetTextEntryMode(false);
            currentMode = MODE_LIST;
            audioClickBack();
            rebuildListMenu();
            return;
        }
        // Backspace: erase one character.
        if (buf.length() > 0) {
            buf.remove(buf.length() - 1);
            dirty = true;
            drawEdit();
        }
        return;
    }
}

static void onExit() {
    if (currentMode == MODE_EDIT) {
        saveCurrentNote();
        inputSetTextEntryMode(false); // don't leak text-entry mode into whatever app opens next
    }
    if (listMenu) {
        delete listMenu;
        listMenu = nullptr;
    }
}

static bool wantsExit() { return exitApp; }

}

AppModule notesAppGet() {
    return { NotesApp::init, NotesApp::tick, NotesApp::handleInput, NotesApp::onExit, NotesApp::wantsExit };
}
