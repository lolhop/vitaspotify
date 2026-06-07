#pragma once

// Front-panel touch for 960x544 UI coordinates.
namespace Touch {

void init();

// Call once per frame before hit-testing.
void update();

// True on finger lift; sets screen coords (0..960, 0..544).
bool consumeTap(float &x, float &y);

// Drop a queued tap (e.g. after navigation debounce).
void discardPendingTap();

// True while finger is down.
bool isDown(float &x, float &y);

}  // namespace Touch
