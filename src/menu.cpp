#include "menu.h"
#include "display.h"
#include "settings.h"
#include "config.h"
#include "audio.h"

Menu::Menu(const char *title_, std::vector<MenuItem> items_)
    : title(title_), items(items_) {}

int Menu::visibleRows() const {
    // Available height = screen minus topbar minus a small bottom margin
    int avail = SCREEN_H - TOPBAR_HEIGHT - 4;
    return avail / ROW_HEIGHT;
}

void Menu::ensureVisible() {
    int vis = visibleRows();
    if (current < scrollOffset) {
        scrollOffset = current;
    } else if (current >= scrollOffset + vis) {
        scrollOffset = current - vis + 1;
    }
}

// Reads bit (x,y) out of a 64x64 (ICON_SRC_SIZE) 1bpp MSB-first bitmap, the
// same layout Adafruit_GFX/Arduino_GFX drawBitmap() expects, and the format
// the lopaka bitmap exporter produces. Each row is padded to a byte boundary.
static inline bool iconBit(const uint8_t *frame, int x, int y) {
    const int rowBytes = (ICON_SRC_SIZE + 7) / 8;
    uint8_t b = pgm_read_byte(&frame[y * rowBytes + (x / 8)]);
    return (b >> (7 - (x % 8))) & 0x01;
}

// Nearest-neighbor downscale of a 64x64 1bpp icon frame into an
// ICON_RENDER_SIZE x ICON_RENDER_SIZE box at (x, y). Only "on" bits are
// plotted (transparent background), so it composites cleanly over the row's
// highlight pill or plain background either way.
static void drawIconFrame(int x, int y, const uint8_t *frame, uint16_t color) {
    for (int dy = 0; dy < ICON_RENDER_SIZE; dy++) {
        int sy = dy * ICON_SRC_SIZE / ICON_RENDER_SIZE;
        for (int dx = 0; dx < ICON_RENDER_SIZE; dx++) {
            int sx = dx * ICON_SRC_SIZE / ICON_RENDER_SIZE;
            if (iconBit(frame, sx, sy)) {
                gfx->drawPixel(x + dx, y + dy, color);
            }
        }
    }
}

void Menu::drawRow(int index, int screenRow, bool isHighlighted) {
    const Theme &t = gSettings.theme();
    int y = TOPBAR_HEIGHT + 2 + screenRow * ROW_HEIGHT;

    if (isHighlighted) {
        if (gSettings.highlightStyle == HIGHLIGHT_OUTLINE) {
            // Outline style: leave the row background alone, just draw an
            // accent-colored border around it. Keeps the row's normal fg
            // text underneath so nothing needs to be redrawn in a different
            // color here.
            gfx->fillRect(6, y, SCREEN_W - 12, ROW_HEIGHT - 4, t.bg);
            gfx->setTextColor(t.accent);
        } else {
            // Filled style (default): rounded pill with accent color, inset from edges
            gfx->fillRoundRect(6, y, SCREEN_W - 12, ROW_HEIGHT - 4, 8, t.accent);
            gfx->setTextColor(t.accentFg);
        }
    } else {
        // Normal row: just clear the background
        gfx->fillRect(6, y, SCREEN_W - 12, ROW_HEIGHT - 4, t.bg);
        gfx->setTextColor(t.fg);
    }

    int textX = 16;
    const MenuItem &item = items[index];

    // Always reset back to the built-in font (setFont(NULL)) once done: gfx's
    // font is shared global state, and every other screen (topbar, app
    // diagnostic views) assumes the compact built-in font unless it says
    // otherwise.
    gfx->setFont(UI_FONT);
    gfx->setTextSize(1);

    if (item.iconFrames && item.iconFrameCount > 0) {
        // Animated bitmap icon: cycles frames only while this row is the
        // settled, highlighted selection; every other row (and this one,
        // whenever it isn't selected) always shows frame 0. the "static
        // image".
        int frame = 0;
        if (isHighlighted) {
            frame = (millis() / ICON_FRAME_MS) % item.iconFrameCount;
        }
        uint16_t iconColor = isHighlighted ? t.accentFg : t.fg;
        int iconY = y + (ROW_HEIGHT - 4 - ICON_RENDER_SIZE) / 2;
        drawIconFrame(textX, iconY, item.iconFrames[frame], iconColor);
        textX += ICON_RENDER_SIZE + 6;
    } else if (item.icon) {
        // Fallback: plain text glyph (used until this item gets a bitmap icon)
        gfx->setCursor(textX, uiFontTextBaselineY(y, ROW_HEIGHT - 4, item.icon));
        gfx->print(item.icon);
        textX += 24;
    }

    // Draw label
    gfx->setCursor(textX, uiFontTextBaselineY(y, ROW_HEIGHT - 4, item.label));
    gfx->print(item.label);
    gfx->setFont(NULL);

    // Outline style draws its border AFTER the label so it sits crisply on
    // top of the row instead of the text potentially overlapping it.
    if (isHighlighted && gSettings.highlightStyle == HIGHLIGHT_OUTLINE) {
        gfx->drawRoundRect(6, y, SCREEN_W - 12, ROW_HEIGHT - 4, 8, t.accent);
    }

    // Subtle bottom separator line for non-highlighted, non-last items
    if (!isHighlighted && index < (int)items.size() - 1) {
        int sepY = y + ROW_HEIGHT - 4;
        gfx->drawFastHLine(20, sepY, SCREEN_W - 40, t.dim);
    }
}

void Menu::drawGlidingHighlight() {
    const Theme &t = gSettings.theme();
    int vis = visibleRows();

    // Only glide if both the row we're leaving and the row we're entering are
    // currently on-screen. If a fast multi-step navigation scrolled one of them
    // off-screen mid-flight, bail out of the glide. but still settle on a
    // real drawn state via draw() instead of just flipping the flag and
    // leaving nothing highlighted at all until the next tick happens to fix
    // it. That gap (no pill, no drawGlidingHighlight call, animating already
    // false) is what showed up as the highlight vanishing for a frame during
    // fast/scrolling navigation.
    int prevScreenRow = previous - scrollOffset;
    int currScreenRow = current - scrollOffset;
    if (prevScreenRow < 0 || prevScreenRow >= vis || currScreenRow < 0 || currScreenRow >= vis) {
        animating = false;
        draw();
        return;
    }

    float progress = float(millis() - animStart) / float(HIGHLIGHT_ANIM_MS);
    if (progress >= 1.0f) progress = 1.0f;

    // Ease-out cubic: fast start, gentle settle. Reads as "physical" rather than linear.
    float inv = 1.0f - progress;
    float eased = 1.0f - (inv * inv * inv);

    int yFrom = TOPBAR_HEIGHT + 2 + prevScreenRow * ROW_HEIGHT;
    int yTo   = TOPBAR_HEIGHT + 2 + currScreenRow * ROW_HEIGHT;
    int y = yFrom + (int)((yTo - yFrom) * eased);

    if (gSettings.highlightStyle == HIGHLIGHT_OUTLINE) {
        // Nothing solid to paint over here
        gfx->drawRoundRect(6, y, SCREEN_W - 12, ROW_HEIGHT - 4, 8, t.accent);
        return;
    }

    gfx->fillRoundRect(6, y, SCREEN_W - 12, ROW_HEIGHT - 4, 8, t.accent);

    // The fill above covers whichever row's label happens to sit under the
    // pill right now with a solid block. This includes the label itself, since
    // it was drawn in the plain fg color before the pill went down. Without
    // this, the label just disappears for the ~140ms.
    int shownIndex = (progress < 0.5f) ? previous : current;
    int shownScreenRow = shownIndex - scrollOffset;
    if (shownScreenRow >= 0 && shownScreenRow < vis && shownIndex >= 0 && shownIndex < (int)items.size()) {
        const MenuItem &item = items[shownIndex];
        int textY = TOPBAR_HEIGHT + 2 + shownScreenRow * ROW_HEIGHT;
        int textX = 16;
        gfx->setTextColor(t.accentFg);
        gfx->setFont(UI_FONT);
        gfx->setTextSize(1);

        if (item.iconFrames && item.iconFrameCount > 0) {
            int iconY = textY + (ROW_HEIGHT - 4 - ICON_RENDER_SIZE) / 2;
            drawIconFrame(textX, iconY, item.iconFrames[0], t.accentFg);
            textX += ICON_RENDER_SIZE + 6;
        } else if (item.icon) {
            gfx->setCursor(textX, uiFontTextBaselineY(textY, ROW_HEIGHT - 4, item.icon));
            gfx->print(item.icon);
            textX += 24;
        }

        gfx->setCursor(textX, uiFontTextBaselineY(textY, ROW_HEIGHT - 4, item.label));
        gfx->print(item.label);
        gfx->setFont(NULL);
    }
}

// Only repaints the row we're leaving + the row we're entering, not the
// whole list. was previously calling draw() every frame which flickered
// like crazy on this no-framebuffer display.
void Menu::drawAnimatingFrame() {
    int vis = visibleRows();
    int prevScreenRow = previous - scrollOffset;
    int currScreenRow = current - scrollOffset;

    if (prevScreenRow < 0 || prevScreenRow >= vis || currScreenRow < 0 || currScreenRow >= vis) {
        animating = false;
        draw(); // fall back to a single full (one-shot, not repeated) redraw
        return;
    }

    int lo = min(prevScreenRow, currScreenRow);
    int hi = max(prevScreenRow, currScreenRow);
    for (int screenRow = lo; screenRow <= hi; screenRow++) {
        drawRow(scrollOffset + screenRow, screenRow, false); // pill drawn on top below
    }
    drawGlidingHighlight();
}

void Menu::drawScrollbar() {
    const Theme &t = gSettings.theme();
    int n = items.size();
    int vis = visibleRows();
    if (n <= vis) return; // no scrollbar needed

    int barAreaTop = TOPBAR_HEIGHT + 4;
    int barAreaH = SCREEN_H - barAreaTop - 4;
    int barX = SCREEN_W - 4;
    int barW = 3;

    // Track
    gfx->fillRoundRect(barX, barAreaTop, barW, barAreaH, 1, t.dim);

    // Thumb
    int thumbH = max(8, barAreaH * vis / n);
    int thumbY = barAreaTop + (barAreaH - thumbH) * scrollOffset / (n - vis);
    gfx->fillRoundRect(barX, thumbY, barW, thumbH, 1, t.accent);
}

void Menu::draw() {
    const Theme &t = gSettings.theme();

    // Clear the menu area
    gfx->fillRect(0, TOPBAR_HEIGHT, SCREEN_W, SCREEN_H - TOPBAR_HEIGHT, t.bg);

    int vis = visibleRows();
    int end = min((int)items.size(), scrollOffset + vis);

    // While the highlight is gliding between rows, draw every row in its plain
    // (unhighlighted) state. the moving pill is painted separately on top.
    // so the destination row doesn't "snap" to highlighted before the pill
    // actually arrives there.
    for (int i = scrollOffset; i < end; i++) {
        int screenRow = i - scrollOffset;
        bool showHighlighted = (i == current) && !animating;
        drawRow(i, screenRow, showHighlighted);
    }

    if (animating) {
        drawGlidingHighlight();
    }

    drawScrollbar();
    needsRedraw = false;
}

void Menu::tick() {
    // Keep the currently-selected row's bitmap icon animating even when
    // there's no navigation happening. but only do the extra redraw work if
    // that row actually has an animated icon, so menus with plain text/glyph
    // icons (the vast majority right now) are completely unaffected and stay
    // fully idle between inputs, exactly as before this feature existed.
    bool iconAnimating = !animating && !needsRedraw &&
                          current >= 0 && current < (int)items.size() &&
                          items[current].iconFrames && items[current].iconFrameCount > 0;

    if (!animating && !needsRedraw && !iconAnimating) return;

    if (iconAnimating && !animating && !needsRedraw) {
        if (millis() - lastIconFrameMs < ICON_FRAME_MS) return;
        lastIconFrameMs = millis();
        // Same deal as drawAnimatingFrame. just the one row, not draw().
        int screenRow = current - scrollOffset;
        int vis = visibleRows();
        if (screenRow >= 0 && screenRow < vis) {
            drawRow(current, screenRow, true);
        }
        return;
    }

    if (animating) {
        // FLICKER: this loop() runs essentially as fast as the board can
        // go (a 2ms delay at the bottom of loop()), so without a cap this was
        // calling draw() dozens of times over a single 140ms glide. each one
        // a full clear-and-redraw of the whole visible list. Capping to
        // ANIM_FRAME_MS keeps the glide just as smooth (it's a ~140ms motion,
        // not a long one) while cutting the redraw traffic drastically, and
        // drawAnimatingFrame() (below) only touches the rows that actually
        // change instead of the whole screen.
        if (millis() - lastAnimFrameMs < ANIM_FRAME_MS) return;
        lastAnimFrameMs = millis();

        drawAnimatingFrame();
        if (millis() - animStart >= HIGHLIGHT_ANIM_MS) {
            animating = false;
            draw(); // settle: one clean full redraw once the glide's done
        }
    } else if (needsRedraw) {
        draw();
    }
}

void Menu::handleInput(const InputResult &in) {
    int n = items.size();
    if (n == 0) return;

    switch (in.type) {
        case InputEvent::NAV_UP:
        case InputEvent::NAV_LEFT: {
            previous = current;
            current = (current - 1 + n) % n;
            ensureVisible();
            animStart = millis();
            animating = true;
            needsRedraw = true;
            audioClickNav();
            draw();
            break;
        }
        case InputEvent::NAV_DOWN:
        case InputEvent::NAV_RIGHT: {
            previous = current;
            current = (current + 1) % n;
            ensureVisible();
            animStart = millis();
            animating = true;
            needsRedraw = true;
            audioClickNav();
            draw();
            break;
        }
        case InputEvent::OK: {
            audioClickOk();
            if (items[current].onEnter) items[current].onEnter();
            break;
        }
        default: break;
    }
}
