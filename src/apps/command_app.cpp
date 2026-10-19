#include "app_api.h"
#include "menu.h"
#include "display.h"
#include "settings.h"
#include "battery.h"
#include "pins.h"
#include "ethernet_usb.h"
#include "touch_scroll.h"
#include "touch_ui.h"
#include <WiFi.h>
#include <SD.h>
#include <Wire.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <vector>
#include <cctype>
#include <algorithm>

namespace CommandApp {
enum Mode { MENU, NICKNAME, DETAILS, CONSOLE };
struct Line { String text; uint16_t color; };
static Mode mode=MENU;
static Menu *menu=nullptr;
static bool exitRequested=false,resuming=false,scanPending=false,connecting=false,dirty=false;
static uint32_t connectStarted=0,lastFrame=0,lastCaret=0;
static String input,nickname,serialLine;
static std::vector<Line> lines;
static TouchScroll scroll;
static bool followOutput=true;
static uint32_t reconnectAt=0;

static void draw();
static void output(const String &message,uint16_t color=0) {
    if(!color) color=gSettings.theme().fg;
    Serial.println(message);
    int start=0;
    do {
        int end=min(start+50,int(message.length()));
        int newline=message.indexOf('\n',start);
        if(newline>=start && newline<end) end=newline;
        lines.push_back({message.substring(start,end),color});
        start=end;
        if(start<int(message.length()) && message[start]=='\n') ++start;
    } while(start<int(message.length()));
    while(lines.size()>180) {lines.erase(lines.begin());scroll.position=max(0.0f,scroll.position-12);}
    scroll.configure(lines.size(),12,28,202);
    if(followOutput) scroll.reset(scroll.limit());
    dirty=true;
}
static bool tokenize(const String &line,std::vector<String> &args) {
    args.clear();String token;char quote=0;bool escaped=false,started=false;
    for(size_t i=0;i<line.length();i++) {
        char c=line[i];
        if(escaped) {token+=c;escaped=false;started=true;continue;}
        if(c==92 && quote) {escaped=true;continue;}
        if(quote) {if(c==quote) quote=0;else token+=c;started=true;continue;}
        if(c=='"' || c=='\'') {quote=c;started=true;continue;}
        if(c==' ' || c=='\t') {if(started) {args.push_back(token);token="";started=false;}}
        else {token+=c;started=true;}
        if(args.size()>8) return false;
    }
    if(quote || escaped) return false;
    if(started) args.push_back(token);
    return true;
}
static String visibleInput(const String &line) {
    std::vector<String> words;String word;char quote=0;bool escaped=false,started=false;
    for(size_t i=0;i<line.length();i++) {
        char c=line[i];
        if(!started && c!=' ' && c!='\t') {
            if(words.size()==3) {
                String command=words[0],arg=words[1];command.toLowerCase();arg.toLowerCase();
                if(command=="wifi" && arg=="connect") return line.substring(0,i)+"********";
                return line;
            }
            started=true;
        }
        if(escaped) {word+=c;escaped=false;continue;}
        if(c==92 && quote) {escaped=true;continue;}
        if(quote) {if(c==quote) quote=0;else word+=c;continue;}
        if(c==34 || c==39) {quote=c;continue;}
        if(c==' ' || c=='\t') {if(started) {words.push_back(word);word="";started=false;}}
        else word+=c;
    }
    return line;
}
static void help() {
    const char *commands[]={
        "module list | health", "ip me | eth | v4 | v6",
        "wifi scan | spect | spam | status", "wifi connect <ssid> [pass] | disconnect",
        "ble scan | monitor | control", "lora ping | beacon | monitor",
        "gpio list | scan | bus | 3v | 5v", "gpio read <configured pin>",
        "chip health | info | display | restart", "chip selftest / input physics checks", "clear | help | exit"
    };
    for(const char *command:commands) output(command);
    output("Quote SSIDs/passwords containing spaces.",gSettings.theme().dim);
    output("Live views: Back returns to this console.",gSettings.theme().dim);
}
static void health() {
    output("ESP32-S3 / running / "+String(ESP.getCpuFreqMHz())+" MHz");
    output("Heap "+String(ESP.getFreeHeap()/1024)+" KB free; largest "+String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)/1024)+" KB");
    output("PSRAM "+String(ESP.getFreePsram()/1024)+" KB free");
    output("Battery ~"+String(batteryPercent())+"% / "+String(batteryMillivolts())+" mV");
    output("Last display transfer "+String(displayLastTransferUs())+" us");
    output("Uptime "+String(millis()/1000)+" s");
}
static std::vector<int> configuredPins() {
    std::vector<int> pins;
    for(size_t i=0;i<GPIO_CUSTOM_PIN_COUNT;i++) pins.push_back(GPIO_CUSTOM_PINS[i]);
    File f=SD.open("/gpio_pins.conf",FILE_READ);
    if(f) {
        while(f.available()) {
            String line=f.readStringUntil('\n');line.trim();
            if(!line.startsWith("CUSTOM_PINS=")) continue;
            String value=line.substring(12);int start=0;
            while(start<int(value.length())) {
                int end=value.indexOf(',',start);if(end<0) end=value.length();
                String token=value.substring(start,end);token.trim();char *tail;
                long pin=strtol(token.c_str(),&tail,10);
                if(token.length() && !*tail && pin>=0 && pin<=48 && !(pin>=22 && pin<=37) && std::find(pins.begin(),pins.end(),pin)==pins.end()) pins.push_back(pin);
                start=end+1;
            }
        }
        f.close();
    }
    return pins;
}
static bool configuredPin(int pin) {
    auto pins=configuredPins();return std::find(pins.begin(),pins.end(),pin)!=pins.end();
}
static void request(int app,const String &tool) {
    if(scanPending || connecting) {output("Wait for the pending network operation.",gSettings.theme().warn);return;}
    output("Opening "+tool+". Back returns here.",gSettings.theme().dim);
    appRequestTool(app,tool);
}
static void execute(String line) {
    line.trim();if(!line.length()) return;
    followOutput=true;output(String(gSettings.consoleNickname)+"@Zyro-Lite $ "+visibleInput(line),gSettings.theme().accent);
    std::vector<String> args;
    if(!tokenize(line,args) || args.empty()) {output("Unclosed quote or invalid command.",gSettings.theme().warn);return;}
    String command=args[0];command.toLowerCase();
    String arg=args.size()>1?args[1]:"";arg.toLowerCase();
    if(command=="help") {help();return;}
    if(command=="clear") {lines.clear();scroll.reset();dirty=true;return;}
    if(command=="exit") {exitRequested=true;return;}
    if(command=="module") {
        if(arg=="list") {
            output("chip / ESP32-S3, flash, PSRAM");output("wifi / 2.4 GHz station and scanner");
            output("ble / discovery, beacons, HID control");output("lora + rf / onboard SX1262 (shared radio)");
            output("gps / MIA-M10Q UART receiver");output("sd / SPI storage; gpio / configured pins");
            output("display / ST7789; touch / GT911");output("keyboard / I2C; trackball / GPIO");
            output("audio / I2S output; eth / experimental USB host");
            output("NRF24 / CC1101 require configured wiring.");
        } else if(arg=="health") {
            health();output("Touch: "+String(inputTouchAvailable()?"GT911 detected":"not detected"));
            output("SD: "+String(SD.cardType()!=CARD_NONE?"mounted":"not mounted"));
            output("Wi-Fi: "+String(WiFi.status()==WL_CONNECTED?"connected":WiFi.getMode()==WIFI_OFF?"off":"idle"));
            int state=loraModuleHealth();output("SX1262: "+String(state<0?"not initialized":state?"initialized":"initialization failed"));
            output("BLE/GPS: initialize through their tools.");
            output("Audio: available; no acoustic self-test.");
            output("Ethernet: "+String(ethernetAdapterPresent()?"adapter detected":"host inactive / no adapter"));
        } else output("Usage: module list | module health");
        return;
    }
    if(command=="ip") {
        if(arg=="eth") output("Ethernet IPv4: "+ethernetIpString()+String(ethernetHasIp()?"":" (no lease)"));
        else if(arg=="v6") {
            if(WiFi.STA.hasGlobalIPv6()) output("IPv6: "+WiFi.STA.globalIPv6().toString());
            else if(WiFi.STA.hasLinkLocalIPv6()) output("Link-local IPv6: "+WiFi.STA.linkLocalIPv6().toString());
            else output("IPv6: no address assigned");
        } else if(arg=="me" || arg=="v4") {
            output("Wi-Fi IPv4: "+WiFi.localIP().toString());
            output("Gateway: "+WiFi.gatewayIP().toString());output("Subnet: "+WiFi.subnetMask().toString());
        } else output("Usage: ip me | eth | v4 | v6");
        return;
    }
    if(command=="wifi") {
        if(arg=="status") {output("SSID: "+WiFi.SSID());output("IPv4: "+WiFi.localIP().toString());if(WiFi.status()==WL_CONNECTED) output("RSSI: "+String(WiFi.RSSI())+" dBm");return;}
        if(arg=="disconnect") {WiFi.disconnect();connecting=false;output("Disconnected.");return;}
        if(arg=="spect" || arg=="spam") {request(1,arg);return;}
        if(scanPending || connecting) {output("A network operation is already running.");return;}
        if(arg=="scan") {
            if(WiFi.getMode()==WIFI_OFF) WiFi.mode(WIFI_STA);
            int result=WiFi.scanNetworks(true,true);
            if(result==WIFI_SCAN_FAILED) output("Wi-Fi scan failed.",gSettings.theme().warn);
            else {scanPending=true;output("Scanning 2.4 GHz...");}
            return;
        }
        if(arg=="connect" && (args.size()==3 || args.size()==4)) {
            if(!args[2].length() || args[2].length()>32 || (args.size()==4 && args[3].length()>63)) {output("SSID max 32 bytes; password max 63 bytes.");return;}
            WiFi.mode(WIFI_STA);WiFi.persistent(false);
            if(args.size()==4 && args[3].length()) WiFi.begin(args[2].c_str(),args[3].c_str());else WiFi.begin(args[2].c_str());
            connecting=true;connectStarted=millis();output("Connecting to "+args[2]+"...");
            return;
        }
        output("Usage: wifi scan | spect | spam | status");output("wifi connect <ssid> [pass] | disconnect");return;
    }
    if(command=="ble") {
        if(arg=="scan" || arg=="monitor" || arg=="control") request(2,arg);else output("Usage: ble scan | monitor | control");return;
    }
    if(command=="lora") {
        if(arg=="ping" || arg=="beacon" || arg=="monitor") request(3,arg);else output("Usage: lora ping | beacon | monitor");return;
    }
    if(command=="gpio") {
        if(arg=="3v") {output("3V3 is a fixed supply rail, not a GPIO mode.");output("GPIO logic is 3.3 V; no rail switch exposed.");return;}
        if(arg=="5v") {output("ESP32 GPIO cannot produce or tolerate 5 V.");output("5 V requires external power/USB VBUS wiring.");return;}
        if(arg=="bus") {
            int found=0;output("I2C devices on SDA18 / SCL8:");
            for(int addr=8;addr<120;addr++) {Wire.beginTransmission(addr);if(Wire.endTransmission()==0) {output("0x"+String(addr,HEX));found++;}yield();}
            output(String(found)+" device(s) acknowledged.");return;
        }
        if(arg=="list") {
            output("Reserved: SPI 9,11,12,13,17,38,39,40,41,45");
            output("I2C 8,18; keyboard IRQ46; touch IRQ16");
            output("Trackball 0,1,2,3,15; battery ADC4");
            output("Audio 5,6,7,14,21,47,48; GPS 43,44");
            output("Power latch10; backlight42");
        }
        if(arg=="list" || arg=="scan") {
            output("Configured user GPIO (read-only inspection):");
            auto pins=configuredPins();
            if(pins.empty()) output("None. No unverified pins are driven or probed.");
            for(int pin:pins) output("GPIO "+String(pin)+": "+String(digitalRead(pin)?"HIGH":"LOW"));
            if(arg=="scan") output("Levels do not prove a physical connection.");
            return;
        }
        if(arg=="read" && args.size()==3) {
            char *end;long pin=strtol(args[2].c_str(),&end,10);
            if(*end || !args[2].length() || !configuredPin(pin)) {output("Pin is not in the verified user GPIO list.");return;}
            output("GPIO "+String(pin)+": "+String(digitalRead(pin)?"HIGH":"LOW"));return;
        }
        output("Usage: gpio list | scan | bus | 3v | 5v");output("gpio read <configured pin>");return;
    }
    if(command=="chip") {
        if(arg=="screen") {
            draw();Serial.println("ZYRO_FRAME 320 240");
            Serial.write((const uint8_t*)displayGetFramebuffer(),SCREEN_W*SCREEN_H*2);Serial.flush();return;
        }
        if(arg=="selftest") {
            TouchScroll test;test.configure(20,34,26,238);
            InputResult e;e.type=InputEvent::TOUCH_DOWN;e.y=180;test.handle(e);
            delay(16);e.type=InputEvent::TOUCH_MOVE;e.y=110;test.handle(e);
            bool follows=int(test.position)==70;
            e.type=InputEvent::TOUCH_UP;bool suppressed=test.handle(e);
            float before=test.position;delay(16);bool moved=test.tick();
            bool inertia=moved && test.position>before;
            test.reset(99999);bool bounds=test.position==test.limit();
            test.reset(34);bool hit=test.itemAt(26)==1 && test.itemAt(238)==-1;
            output(String(follows&&suppressed&&inertia&&bounds&&hit?"PASS":"FAIL")+" / scroll drag, momentum, bounds, hit targets");return;
        }
        if(arg=="display") {draw();displayFlushIfChanged();float fps=touchUiBenchmark(args.size()>2 && args[2]=="capture");output("Quick-panel measured FPS: "+String(fps,1));return;}
        if(arg=="restart") {output("Restarting...");draw();displayFlushIfChanged();delay(150);ESP.restart();return;}
        if(arg=="health") {health();return;}
        if(arg=="info") {
            output(String(ESP.getChipModel())+" rev "+String(ESP.getChipRevision()));
            output(String(ESP.getChipCores())+" cores / SDK "+ESP.getSdkVersion());
            output("Flash "+String(ESP.getFlashChipSize()/1048576)+" MB / PSRAM "+String(ESP.getPsramSize()/1048576)+" MB");return;
        }
        output("Usage: chip health | info | display | restart");return;
    }
    output("Unknown command. Type help.",gSettings.theme().warn);
}
static void launch() {
    mode=CONSOLE;input="";inputSetTextEntryMode(true);followOutput=true;
    scroll.configure(lines.size(),12,28,202);scroll.reset(scroll.limit());
    if(lines.empty()) {output("Zyro-Lite console / "+String(FW_VERSION));output("Type help for commands.",gSettings.theme().dim);}
    dirty=true;draw();
}
static void draw() {
    const Theme &t=gSettings.theme();
    if(mode==MENU) {if(menu) menu->draw();return;}
    uiAppSurface();
    if(mode==NICKNAME) {
        gfx->setTextColor(t.accent);gfx->setCursor(10,29);gfx->print("Console nickname");
        gfx->setTextColor(t.fg);gfx->setCursor(12,66);gfx->print(nickname+"_");
        gfx->setTextColor(t.dim);gfx->setCursor(12,94);gfx->print("Letters, digits, _ or - / max 16");
        gfx->setCursor(12,118);gfx->print("Enter saves / Esc cancels");return;
    }
    if(mode==DETAILS) {
        gfx->setTextColor(t.accent);gfx->setCursor(10,29);gfx->print("Command prompt");
        const String details[]={String(gSettings.consoleNickname)+"@Zyro-Lite",String(ESP.getChipModel())+" / "+String(ESP.getCpuFreqMHz())+" MHz",
            "Flash "+String(ESP.getFlashChipSize()/1048576)+" MB / PSRAM "+String(ESP.getPsramSize()/1048576)+" MB",
            "Wi-Fi "+WiFi.localIP().toString(),"Touch: "+String(inputTouchAvailable()?"detected":"unavailable"),
            "Firmware " FW_VERSION,"Commands use live chip APIs.","Tools return here with Back."};
        gfx->setTextColor(t.fg);int y=54;for(const auto &line:details) {gfx->setCursor(12,y);gfx->print(line);y+=20;}return;
    }
    gfx->fillRect(0,TOPBAR_HEIGHT,SCREEN_W,SCREEN_H-TOPBAR_HEIGHT,t.bg);
    scroll.configure(lines.size(),12,28,202);uiBeginViewport(28,202);
    int first=int(scroll.position)/12;
    for(int i=first;i<min(int(lines.size()),first+17);i++) {
        gfx->setTextColor(lines[i].color);gfx->setCursor(6,28+i*12-int(scroll.position));gfx->print(lines[i].text);
    }
    uiEndViewport();gfx->drawFastHLine(6,204,308,t.dim);
    gfx->setTextColor(t.accent);gfx->setCursor(6,208);gfx->print(String(gSettings.consoleNickname)+"@Zyro-Lite");
    String shown=visibleInput(input);if(shown.length()>48) shown=shown.substring(shown.length()-48);
    gfx->setTextColor(t.fg);gfx->setCursor(6,223);gfx->print("> "+shown+String((millis()/500)%2?"_":" "));
    dirty=false;
}
static void init() {
    exitRequested=false;
    mode=MENU;inputSetTextEntryMode(false);
    if(menu) delete menu;
    menu=new Menu("Command prompt",{
        {"Launch Console",nullptr,[](){launch();}},
        {"Nickname",nullptr,[](){mode=NICKNAME;nickname=gSettings.consoleNickname;inputSetTextEntryMode(true);draw();}},
        {"Important details",nullptr,[](){mode=DETAILS;draw();}},
        {"Back",nullptr,[](){exitRequested=true;}}
    });
    if(resuming) {resuming=false;launch();return;}
    menu->draw();
}
static void submit() {
    String command=input;input="";execute(command);dirty=true;
    // Do not keep a second password copy after execution.
    command="";
}
static void tick() {
    if(mode==MENU) {if(menu) menu->tick();return;}
    if(mode!=CONSOLE) return;
    while(Serial.available()) {
        char c=Serial.read();
        if(c=='\n') {input=serialLine;serialLine="";submit();}
        else if(c!='\r' && serialLine.length()<192) serialLine+=c;
    }
    if(scanPending) {
        int n=WiFi.scanComplete();
        if(n!=WIFI_SCAN_RUNNING) {
            scanPending=false;
            if(n<0) output("Scan failed.",gSettings.theme().warn);
            else {
                output(String(n)+" access points:");
                for(int i=0;i<min(n,40);i++) output(WiFi.SSID(i)+" / "+String(WiFi.RSSI(i))+" dBm / ch "+String(WiFi.channel(i))+" / "+(WiFi.encryptionType(i)==WIFI_AUTH_OPEN?"open":"secured"));
                if(n>40) output("Showing the first 40 results.");
            }
            WiFi.scanDelete();
        }
    }
    if(connecting) {
        if(WiFi.status()==WL_CONNECTED) {connecting=false;output("Connected / "+WiFi.localIP().toString(),gSettings.theme().ok);}
        else if(millis()-connectStarted>15000) {connecting=false;WiFi.disconnect();output("Connection timed out.",gSettings.theme().warn);}
    }
    if(scroll.tick()) dirty=true;
    if(millis()-lastCaret>=500) {lastCaret=millis();dirty=true;}
    if(dirty && millis()-lastFrame>=16) {lastFrame=millis();draw();}
}
static void handleInput(const InputResult &in) {
    if(mode==MENU) {if(in.type==InputEvent::BACK) exitRequested=true;else if(menu) menu->handleInput(in);return;}
    if(mode==DETAILS) {if(in.type==InputEvent::BACK || in.type==InputEvent::OK || in.type==InputEvent::TOUCH_TAP) {mode=MENU;menu->forceRedraw();menu->draw();}return;}
    if(mode==NICKNAME) {
        if(in.type==InputEvent::CHAR && nickname.length()<16 && (isalnum((unsigned char)in.ch)||in.ch=='_'||in.ch=='-')) nickname+=in.ch;
        if(in.type==InputEvent::BACK) {if(in.ch==0x1B) {mode=MENU;inputSetTextEntryMode(false);menu->draw();return;}if(nickname.length()) nickname.remove(nickname.length()-1);}
        if(in.type==InputEvent::OK && nickname.length()) {nickname.toCharArray(gSettings.consoleNickname,sizeof(gSettings.consoleNickname));settingsSave();mode=MENU;inputSetTextEntryMode(false);menu->draw();return;}
        draw();return;
    }
    if(scroll.handle(in)) {followOutput=false;dirty=true;return;}
    if(in.type==InputEvent::CHAR && input.length()<192 && in.ch>=32) {input+=in.ch;dirty=true;}
    else if(in.type==InputEvent::BACK) {
        if(in.ch==0x1B || !in.ch) {mode=MENU;inputSetTextEntryMode(false);menu->draw();return;}
        if(input.length()) input.remove(input.length()-1);dirty=true;
    } else if(in.type==InputEvent::OK || (in.type==InputEvent::TOUCH_TAP && in.y>=204)) submit();
    else if(in.type==InputEvent::NAV_UP) {followOutput=false;scroll.reset(scroll.position-36);dirty=true;}
    else if(in.type==InputEvent::NAV_DOWN) {scroll.reset(scroll.position+36);followOutput=scroll.position>=scroll.limit();dirty=true;}
}
static void onExit() {
    inputSetTextEntryMode(false);input="";serialLine="";
    if(scanPending) {esp_wifi_scan_stop();WiFi.scanDelete();scanPending=false;}
    if(connecting) {WiFi.disconnect();connecting=false;}
}
static bool wantsExit() {return exitRequested;}
}
void commandResume() {CommandApp::resuming=true;}
AppModule commandAppGet() {return {CommandApp::init,CommandApp::tick,CommandApp::handleInput,CommandApp::onExit,CommandApp::wantsExit};}
