# Run from the repository root. Requires pyserial. Captures remain under ignored build/.
import serial,time,re,argparse
parser=argparse.ArgumentParser(description="Hardware console checks; requires flashed firmware and restarts the device.")
parser.add_argument("--port",default="COM10")
options=parser.parse_args()
Path("build").mkdir(exist_ok=True)
from pathlib import Path
port=serial.Serial(options.port,115200,timeout=0.1)
port.dtr=True
log=bytearray()
def read_for(duration):
    data=bytearray();end=time.monotonic()+duration
    while time.monotonic()<end:
        data.extend(port.read(max(1,port.in_waiting)))
    log.extend(data)
    return data.decode('utf-8',errors='replace')
def run(command,duration=0.35):
    port.write((command+'\n').encode());return read_for(duration)
def frame(command,name):
    port.write((command+'\n').encode())
    prefix=bytearray();end=time.monotonic()+10
    while time.monotonic()<end:
        line=port.readline()
        if line.strip()==b'ZYRO_FRAME 320 240': break
        prefix.extend(line)
    else: raise RuntimeError('Frame header missing: '+command+' / '+prefix.decode(errors='replace'))
    payload=bytearray()
    while len(payload)<153600 and time.monotonic()<end:
        payload.extend(port.read(153600-len(payload)))
    if len(payload)!=153600: raise RuntimeError('Incomplete framebuffer')
    Path('build/'+name+'.rgb565').write_bytes(payload)
    log.extend(prefix)
    return prefix.decode(errors='replace')+read_for(1.8)
read_for(0.6)
run('console',0.7)
checks={}
checks['module enumeration']='ESP32-S3' in run('module list')
checks['hardware health']='GT911 detected' in run('module health')
checks['IPv4 command']='Wi-Fi IPv4:' in run('ip me')
checks['Ethernet IP command']='Ethernet IPv4:' in run('ip eth')
checks['IPv6 command']='IPv6' in run('ip v6')
checks['scroll physics self-test']='PASS / scroll drag' in run('chip selftest',0.5)
checks['3V rail command']='fixed supply rail' in run('gpio 3v')
checks['5V hardware constraint']='cannot produce' in run('gpio 5v')
checks['GPIO inspection']='Configured user GPIO' in run('gpio scan')
private=run('WIFI   CONNECT "" "secretNeverPrint123"')
checks['quoted parsing and password masking']='SSID max 32 bytes' in private and 'secretNeverPrint123' not in private
checks['unclosed quote handling']='Unclosed quote' in run('wifi connect "unterminated')
scan=run('wifi scan',6)
checks['asynchronous Wi-Fi scan']='access points:' in scan
capture=frame('chip display capture','quick-panel')
match=re.search(r'Quick-panel measured FPS: ([0-9.]+)',capture)
checks['panel benchmark']=bool(match)
frame('chip screen','command-prompt')
run('ble scan',5)
run('console',0.8)
checks['BLE tool round-trip']='ESP32-S3' in run('chip health')
run('lora monitor',1.3)
run('console',0.8)
checks['LoRa tool round-trip']='ESP32-S3' in run('chip health')
boot=run('chip restart',6)
checks['chip restart and boot']='rootMenuInit done - reached loop()' in boot and '[touch] GT911: OK' in boot
run('console',0.5)
run('exit',0.5)
port.close()
text=log.decode('utf-8',errors='replace')
Path('build/console-integration.log').write_text(text,encoding='utf-8')
checks['no credential leakage']='secretNeverPrint123' not in text
checks['no runtime panic']=not any(s in text.lower() for s in ['guru meditation','backtrace:','task watchdog','abort()'])
for name,passed in checks.items(): print(('PASS' if passed else 'FAIL')+' / '+name)
if match: print('Measured quick-panel FPS: '+match.group(1))
raise SystemExit(0 if all(checks.values()) else 1)
