#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>
#include "input.h"
#include "config.h"

// FYI: iconFrames/iconFrameCount are intentionally the LAST two fields,
// after onEnter. Every existing call site across the app files brace-inits a
// MenuItem with just { label, icon, onEnter } (3 fields). in C++ aggregate
// initialization, any trailing fields you don't list are value-initialized
// (nullptr / 0), so all of that existing code keeps compiling and behaving
// exactly as before, with no icon. Only add the two extra fields at call
// sites that actually want an animated icon.
struct MenuItem {
    const char *label;
    const char *icon;    // short 1-2 char glyph shown before label (can be nullptr)
    std::function<void()> onEnter;  // called when OK is pressed on this item
    const uint8_t (*iconFrames)[ICON_SRC_SIZE * ICON_SRC_SIZE / 8]; // optional animated icon, else nullptr
    int iconFrameCount;
};

class Menu {
public:
    Menu(const char *title, std::vector<MenuItem> items);

    void draw();       
    void tick();                 // advances the glide animation, call every loop
    void handleInput(const InputResult &in);

    int selected() const { return current; }

    // Forces the next tick() to do a full draw(). Used by callers that just
    // painted over the whole screen (e.g. the alert overlay) and need the
    // menu to repaint itself from scratch afterwards, since this display has
    // no framebuffer to restore what was underneath automatically.
    void forceRedraw() { needsRedraw = true; }

private:
    const char *title;
    std::vector<MenuItem> items;

    int current = 0;
    int previous = 0;
    int scrollOffset = 0;       // first visible item index
    uint32_t animStart = 0;
    bool animating = false;
    bool needsRedraw = true;
    uint32_t lastIconFrameMs = 0;
    uint32_t lastAnimFrameMs = 0;

    int visibleRows() const;     // how many rows fit on screen
    void ensureVisible();        // adjust scrollOffset so current is on-screen
    void drawRow(int index, int screenRow, bool isHighlighted);
    void drawGlidingHighlight();  // draws the eased, sliding selection pill mid-animation
    void drawAnimatingFrame();    // repaints ONLY the rows affected by the glide, not the whole list
    void drawScrollbar();
};

void rootMenuInit();
void rootMenuTick();
void rootMenuHandleInput(const InputResult &in);
void rootMenuForceRedraw(); 
