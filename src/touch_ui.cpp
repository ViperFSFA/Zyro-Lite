#include "touch_ui.h"
#include "display.h"
#include "settings.h"
#include "battery.h"
#include "cursor.h"
#include "config.h"
#include <WiFi.h>
#include "wifi_full_icon.h"
#include "fonts/JetBrainsMono8pt7b.h"
#include <cstring>
#include <cmath>

static bool opened=false,held=false,fromTop=false,dragSheet=false,dirty=true;
static int startX=0,startY=0;
static float reveal=0,target=0,speed=0,startReveal=0;
static uint32_t lastFrame=0,frames=0,animationStart=0,presentedFrames=0;
static int previousEdge=-1;
static uint16_t *backing=nullptr,*blurred=nullptr,*sheet=nullptr;
static uint16_t blend(uint16_t a,uint16_t b,int alpha) {
    int r=(((a>>11)&31)*(255-alpha)+((b>>11)&31)*alpha)/255;
    int g=(((a>>5)&63)*(255-alpha)+((b>>5)&63)*alpha)/255;
    int bl=((a&31)*(255-alpha)+(b&31)*alpha)/255;
    return (r<<11)|(g<<5)|bl;
}
static bool capture() {
    const size_t bytes=SCREEN_W*SCREEN_H*2;
    if(!backing) backing=(uint16_t*)ps_malloc(bytes);
    if(!blurred) blurred=(uint16_t*)ps_malloc(bytes);
    if(!sheet) sheet=(uint16_t*)ps_malloc(bytes);
    if(!backing || !blurred || !sheet) return false;
    cursorErase(); memcpy(backing,displayGetFramebuffer(),bytes);
    // Blur once at quarter resolution; animation reuses the cached result.
    uint16_t *small=(uint16_t*)malloc(80*60*2),*tmp=(uint16_t*)malloc(80*60*2);
    if(!small || !tmp) { free(small);free(tmp);return false; }
    for(int y=0;y<60;y++) for(int x=0;x<80;x++) {
        int r=0,g=0,b=0;
        for(int j=0;j<4;j++) for(int i=0;i<4;i++) {
            uint16_t c=backing[(y*4+j)*320+x*4+i]; r+=(c>>11)&31;g+=(c>>5)&63;b+=c&31;
        }
        small[y*80+x]=((r/16)<<11)|((g/16)<<5)|(b/16);
    }
    for(int pass=0;pass<3;pass++) {
        for(int y=0;y<60;y++) for(int x=0;x<80;x++) {
            int r=0,g=0,b=0;
            for(int j=-2;j<=2;j++) for(int i=-2;i<=2;i++) {
                uint16_t c=small[constrain(y+j,0,59)*80+constrain(x+i,0,79)];
                r+=(c>>11)&31;g+=(c>>5)&63;b+=c&31;
            }
            tmp[y*80+x]=((r/25)<<11)|((g/25)<<5)|(b/25);
        }
        uint16_t *swap=small;small=tmp;tmp=swap;
    }
    for(int y=0;y<240;y++) for(int x=0;x<320;x++)
        blurred[y*320+x]=blend(small[(y/4)*80+x/4],gSettings.theme().bg,125);
    free(small);free(tmp);return true;
}
static void closePanel() {
    if(!opened) return;
    if(opened && backing) memcpy(displayGetFramebuffer(),backing,SCREEN_W*SCREEN_H*2);
    opened=false;held=false;cursorInvalidate();
    if(frames>4) Serial.printf("[quick-panel] %u animation frames / %u ms\n",frames,millis()-animationStart);
}
void touchUiDismiss() { closePanel();fromTop=false;held=false; }
bool touchUiActive() { return opened; }
static void brightness(int x) {
    uint8_t value=constrain(map(x,18,302,10,255),10L,255L);
    if(value!=gSettings.brightness) { gSettings.brightness=value;displaySetBacklight(value);dirty=true; }
}
bool touchUiHandle(InputResult &in) {
    if(opened && in.type==InputEvent::BACK) { target=0;held=false;return true; }
    if(in.type==InputEvent::TOUCH_DOWN) {
        held=true;startX=in.x;startY=in.y;startReveal=reveal;
        fromTop=!opened && in.y<TOPBAR_HEIGHT;
        dragSheet=opened && (in.y<34 || in.y>198);
        if(fromTop) {
            if(!capture()) { fromTop=false;held=false;return true; }
            opened=true;reveal=target=speed=0;dirty=true;previousEdge=-1;frames=0;animationStart=millis();
        }
        return opened;
    }
    if(in.type==InputEvent::TOUCH_MOVE) {
        if(!opened) return false;
        if(fromTop) { reveal=constrain((in.y-startY)/170.0f,0.0f,1.03f);target=reveal; }
        else if(dragSheet) { reveal=constrain(startReveal+(in.y-startY)/180.0f,0.0f,1.03f);target=reveal; }
        else if(startY>=157 && startY<194) brightness(in.x);
        return true;
    }
    if(in.type!=InputEvent::TOUCH_UP) return opened;
    if(!held) return true;
    held=false;
    int dx=in.x-startX,dy=in.y-startY;
    if(opened) {
        if(fromTop || dragSheet) { target=reveal>0.3f?1:0;fromTop=dragSheet=false;return true; }
        if(startY>=157 && startY<194) { brightness(in.x);settingsSave();return true; }
        if(abs(dx)>10 || abs(dy)>10) return true;
        if(in.y>=44 && in.y<94) {
            if(in.x<160) { if(WiFi.getMode()==WIFI_OFF) WiFi.mode(WIFI_STA);else WiFi.mode(WIFI_OFF); }
            else gSettings.soundEnabled=!gSettings.soundEnabled;
        } else if(in.y>=104 && in.y<146) {
            if(in.x<160) { gSettings.keyboardBacklight=!gSettings.keyboardBacklight;keyboardSetBacklight(gSettings.keyboardBacklight); }
            else gSettings.batterySaver=!gSettings.batterySaver;
        } else if(in.y>=204) { target=0;return true; }
        else return true;
        dirty=true;settingsSave();return true;
    }
    if(startY<TOPBAR_HEIGHT) return true;
    // Content lists receive the complete gesture; only a real tap becomes a click.
    if(abs(dx)<=7 && abs(dy)<=7) in.type=InputEvent::TOUCH_TAP;
    else if(startX<18 && dx>45 && abs(dy)<24) in.type=InputEvent::BACK;
    return false;
}
static void glass(int x,int y,int w,int h,bool active) {
    const Theme &t=gSettings.theme();
    uint16_t tint=active?t.accent:t.topbarBg;
    uint16_t *fb=displayGetFramebuffer();
    const int radius=14;
    for(int row=0;row<h;row++) {
        int d=row<radius?radius-row:row>=h-radius?row-(h-radius-1):0;
        int inset=d?radius-int(sqrtf(float(radius*radius-d*d))):0;
        for(int col=inset;col<w-inset;col++) {
            int p=(y+row)*SCREEN_W+x+col;
            fb[p]=blend(fb[p],tint,active?230:185);
        }
    }
}
static void buildSheet() {
    const Theme &t=gSettings.theme();
    memcpy(displayGetFramebuffer(),blurred,SCREEN_W*SCREEN_H*2);
    gfx->setFont(NULL);gfx->setTextWrap(false);gfx->setTextSize(1);
    auto text=[&](int x,int y,const String &s,uint16_t c){gfx->setFont(NULL);gfx->setTextColor(c);gfx->setCursor(x,y);gfx->print(s);};
    auto label=[&](int x,int y,const char *s,uint16_t c){gfx->setFont(&JetBrainsMono8pt7b);gfx->setTextColor(c);gfx->setCursor(x,y);gfx->print(s);gfx->setFont(NULL);};
    label(16,24,"Quick panel",t.fg);text(267,16,String(batteryPercent())+"%",t.fg);
    bool wifi=WiFi.getMode()!=WIFI_OFF;
    glass(12,44,144,50,wifi);glass(164,44,144,50,gSettings.soundEnabled);
    label(26,66,"Wi-Fi",wifi?t.accentFg:t.fg);
    gfx->drawBitmap(125,53,wifi_full_icon_bits,19,16,wifi?t.accentFg:t.dim);
    text(26,75,WiFi.status()==WL_CONNECTED?WiFi.SSID().substring(0,18):wifi?"Not connected":"Off",wifi?t.accentFg:t.dim);
    label(178,66,"Sound",gSettings.soundEnabled?t.accentFg:t.fg);
    uint16_t speaker=gSettings.soundEnabled?t.accentFg:t.dim;
    gfx->fillRect(277,58,4,6,speaker);gfx->fillTriangle(280,58,287,53,287,69,speaker);
    if(gSettings.soundEnabled) {gfx->drawLine(291,55,295,59,speaker);gfx->drawLine(295,59,295,63,speaker);gfx->drawLine(295,63,291,67,speaker);}
    else {gfx->drawLine(291,57,297,65,speaker);gfx->drawLine(297,57,291,65,speaker);}
    text(178,75,gSettings.soundEnabled?"On":"Muted",gSettings.soundEnabled?t.accentFg:t.dim);
    glass(12,104,144,42,gSettings.keyboardBacklight);glass(164,104,144,42,gSettings.batterySaver);
    label(24,130,"Keyboard",gSettings.keyboardBacklight?t.accentFg:t.fg);
    label(177,130,"Power saver",gSettings.batterySaver?t.accentFg:t.fg);
    glass(12,157,296,36,false);
    int width=map(gSettings.brightness,10,255,26,286);
    gfx->fillRoundRect(17,162,width,26,13,t.fg);
    gfx->drawCircle(30,175,4,t.bg);
    text(248,172,String(gSettings.brightness*100/255)+"%",width>258?t.bg:t.fg);
    gfx->fillRoundRect(140,216,40,3,2,t.fg);
    memcpy(sheet,displayGetFramebuffer(),SCREEN_W*SCREEN_H*2);dirty=false;
}
void touchUiDraw() {
    if(!opened) return;
    uint32_t now=millis();if(now-lastFrame<15) return;
    float dt=min(uint32_t(32),now-lastFrame)/16.0f;lastFrame=now;
    bool settling=!held && (fabsf(target-reveal)>0.001f || fabsf(speed)>0.001f);
    if(!held) {
        speed+=(target-reveal)*0.28f*dt;speed*=powf(0.48f,dt);reveal+=speed*dt;
        if(fabsf(target-reveal)<0.002f && fabsf(speed)<0.002f) { reveal=target;speed=0; }
        if(target==0 && reveal<=0.003f) { closePanel();displayFlushDirtyRows();return; }
    }
    if(!dirty && !held && !settling) return;
    int offset=int(lroundf((reveal-1.0f)*SCREEN_H));
    bool rebuild=dirty;
    if(rebuild) buildSheet();
    if(offset!=previousEdge || rebuild) {
        uint16_t *fb=displayGetFramebuffer();
        // Translate the complete cached sheet: header, cards, slider, and handle
        // stay in one coordinate space throughout the downward entrance and upward exit.
        memcpy(fb,blurred,SCREEN_W*SCREEN_H*2);
        int sourceTop=max(0,-offset),destTop=max(0,offset);
        int rows=min(SCREEN_H-sourceTop,SCREEN_H-destTop);
        if(rows>0) memcpy(fb+destTop*SCREEN_W,sheet+sourceTop*SCREEN_W,rows*SCREEN_W*2);
        if(displayFlushDirtyRows()) ++presentedFrames;
    }
    previousEdge=offset;
    if(settling || held) ++frames;
}

float touchUiBenchmark(bool captureFrame) {
    InputResult in;in.type=InputEvent::TOUCH_DOWN;in.x=150;in.y=4;touchUiHandle(in);
    if(!opened) return 0;
    // Separate one-time blur/layout preparation from sustained animation throughput.
    uint32_t prepare=millis();while(previousEdge==-1 && millis()-prepare<500) {touchUiDraw();delay(1);}
    uint32_t first=presentedFrames,started=millis();
    while(millis()-started<1000) {
        in.type=InputEvent::TOUCH_MOVE;in.y=4+int((millis()-started)*180/1000);touchUiHandle(in);
        touchUiDraw();delay(1);
    }
    float fps=(presentedFrames-first)*1000.0f/(millis()-started);
    in.type=InputEvent::TOUCH_UP;touchUiHandle(in);
    uint32_t settle=millis();while(millis()-settle<280) {touchUiDraw();delay(1);}
    if(captureFrame) {
        Serial.println("ZYRO_FRAME 320 240");
        Serial.write((const uint8_t*)displayGetFramebuffer(),SCREEN_W*SCREEN_H*2);Serial.flush();
    }
    in.type=InputEvent::BACK;touchUiHandle(in);
    settle=millis();while(opened && millis()-settle<800) {touchUiDraw();delay(1);}
    touchUiDismiss();return fps;
}
