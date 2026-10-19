#pragma once
#include "input.h"

struct AppModule {
    void (*init)();
    void (*tick)();
    void (*handleInput)(const InputResult &in);
    void (*onExit)();
    bool (*wantsExit)();
};

AppModule wifiAppGet();
AppModule bleAppGet();
AppModule loraAppGet();
AppModule rfAppGet();
AppModule gpsAppGet();
AppModule ethernetAppGet();
AppModule gamesAppGet();
AppModule filesAppGet();
AppModule settingsAppGet();
AppModule gpioAppGet();

AppModule commandAppGet();
void commandResume();
void appRequestTool(int appIndex,const String &tool);
bool wifiOpenTool(const String &tool);
bool bleOpenTool(const String &tool);
bool loraOpenTool(const String &tool);
int loraModuleHealth();
