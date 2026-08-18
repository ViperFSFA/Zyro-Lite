#pragma once
#include <Arduino.h>

// On-screen pointer for "Trackball: Cursor" mode (see settings.h's
// trackballCursor). inputPoll() emits CURSOR_MOVE events instead of
// NAV_UP/DOWN/LEFT/RIGHT while that setting is on, but until now nothing
// ever drew or moved an actual cursor sprite in response - this is that
// piece. Uses the canvas framebuffer directly (see display.h's
// displayGetFramebuffer()) to save/restore the small rectangle of pixels
// under the sprite each frame, the same way a classic software mouse
// pointer overlay works, since this display has no other mechanism to
// "un-draw" something without knowing what was underneath it.

void cursorInit();                    // call once after displayInit(), from setup()
void cursorMove(int dx, int dy);      // called for every CURSOR_MOVE input event
void cursorSuppress(bool suppressed); // hide + stop compositing (e.g. while BLE
                                       // Remote Control is using the trackball
                                       // for its own HID mouse output instead)
void cursorInvalidate();              // drop any pending restore without touching
                                       // the framebuffer - use when something else
                                       // (e.g. the SD-removed alert) just overwrote
                                       // the whole screen out from under us
void cursorTick();                    // call every loop() iteration, after all other
                                       // drawing for that frame, right before flush()
