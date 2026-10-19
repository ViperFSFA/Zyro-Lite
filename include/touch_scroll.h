#pragma once
#include "input.h"
#include <math.h>

// Pixel scroll physics shared by lists. Units: pixels and pixels/millisecond.
class TouchScroll {
public:
    float position = 0;
    float velocity = 0;
    bool dragging = false;
    bool moved = false;
    int rowHeight = 34, top = 26, bottom = 238, count = 0;
    void configure(int n,int h,int y0,int y1) {
        count=n; rowHeight=h; top=y0; bottom=y1;
        position=constrain(position,0.0f,limit());
    }
    float limit() const { return max(0,count*rowHeight-(bottom-top)); }
    void reset(float value=0) { position=constrain(value,0.0f,limit()); velocity=0; dragging=false; moved=false; }
    int itemAt(int y) const { return y<top || y>=bottom ? -1 : int((y-top+position)/rowHeight); }
    bool handle(const InputResult &in) {
        uint32_t now=millis();
        if(in.type==InputEvent::TOUCH_DOWN) {
            if(in.y<top || in.y>=bottom) return false;
            dragging=true; moved=false; originY=lastY=in.y; originPosition=position;
            velocity=0; lastMove=lastTick=now; return true;
        }
        if(in.type==InputEvent::TOUCH_MOVE && dragging) {
            if(abs(in.y-originY)>5) moved=true;
            float next=constrain(originPosition+originY-in.y,0.0f,limit());
            uint32_t dt=max(uint32_t(1),now-lastMove);
            if(in.y!=lastY) { velocity=0.55f*velocity+0.45f*(lastY-in.y)/dt; lastMove=now; }
            position=next; lastY=in.y; return true;
        }
        if((in.type==InputEvent::TOUCH_UP || in.type==InputEvent::TOUCH_TAP) && dragging) {
            dragging=false;
            if(now-lastMove>140 || !moved) velocity=0;
            velocity=constrain(velocity,-2.8f,2.8f); lastTick=now;
            return moved || in.type==InputEvent::TOUCH_UP;
        }
        return false;
    }
    bool tick() {
        uint32_t now=millis(); float dt=min(uint32_t(32),now-lastTick); lastTick=now;
        if(dragging) return false;
        if(fabsf(velocity)<0.015f) { velocity=0; return false; }
        float old=position;
        position=constrain(position+velocity*dt,0.0f,limit());
        velocity*=powf(0.92f,dt/16.0f);
        if(position==0 || position==limit()) velocity=0;
        return int(old)!=int(position);
    }
private:
    int originY=0,lastY=0;
    float originPosition=0;
    uint32_t lastMove=0,lastTick=0;
};
