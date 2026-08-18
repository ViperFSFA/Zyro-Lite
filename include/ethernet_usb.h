#pragma once
#include <Arduino.h>

// The T-Deck Plus has exactly ONE USB-C port, wired to the ESP32-S3's
// built-in USB-OTG (full-speed) peripheral. the same port used for
// flashing and the Serial console. That peripheral can run in USB
// DEVICE mode (what gives you Serial-over-USB / firmware upload) or
// USB HOST mode (what a USB Ethernet adapter needs). never both at
// once. There is only one physical USB PHY on this board.


void ethernetInit();            // switch USB-OTG to host mode, start enumeration + DHCP
void ethernetTick();         
void ethernetShutdown();   

bool   ethernetAdapterPresent();  // a CDC-ECM device is enumerated & claimed
bool   ethernetLinkUp();          // link (carrier) detected
bool   ethernetHasIp();           // DHCP lease obtained
String ethernetIpString();        // "0.0.0.0" if none yet
String ethernetAdapterName();     // USB product string, or "-" if none

// Fires on any state change (adapter plugged/unplugged, link up/down, IP
// obtained) so the UI can redraw without polling every field every frame.
bool ethernetConsumeDirtyFlag();
