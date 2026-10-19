#pragma once
#include "input.h"
bool touchUiHandle(InputResult &in);
bool touchUiActive();
void touchUiDraw();

void touchUiDismiss();

float touchUiBenchmark(bool captureFrame=false);
