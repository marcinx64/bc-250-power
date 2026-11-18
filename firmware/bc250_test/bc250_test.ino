// SPDX-License-Identifier: GPL-3.0-or-later
// BC-250 power controller - bench test firmware
// Board: Seeed XIAO ESP32-C3 (Arduino-ESP32 core 2.x or 3.x)
//
// Starts its own WiFi network and serves a test page:
//   WiFi:     BC250-Test    password: bc250test
//   Page:     http://192.168.4.1
//
// Nothing switches by itself. Every output is LOW after boot and only
// changes when you press a button on the page.

#include <WiFi.h>
#include <WebServer.h>
#include "driver/gpio.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ---- Pin map (matches the whiteboard) ---------------------------------
const int PIN_PSON   = 3;   // D1  -> 220R -> U1 pin 1   (PS_ON drive)
const int PIN_BUZZER = 4;   // D2  -> buzzer +
const int PIN_SENSE  = 5;   // D3  <- U1 pin 6           (BC-250 LED sense, LOW = lit)
const int PIN_PWR    = 6;   // D4  -> 220R -> U2 pin 3   (PWR button)
const int PIN_RST    = 7;   // D5  -> 220R -> U2 pin 1   (RST button)
const int PIN_SWITCH = 20;  // D7  <- front switch contact (LOW = pressed)
const int PIN_LED    = 10;  // D10 -> front switch LED +

// The buzzer is the active type (from a Grove Buzzer v1.2): it sounds
// whenever its pin is HIGH, so beeps are plain on/off, no tone needed.
// Set to 0 to silence it.
#define BUZZER_OK 1

const unsigned long BEEP_ON_MS  = 250;  // length of one beep
const unsigned long BEEP_GAP_MS = 200;  // silence between beeps

const unsigned long PULSE_MS = 500;   // length of a PWR / RST "press"

WebServer server(80);

bool psOn = false;
unsigned long pwrOffAt = 0;
unsigned long rstOffAt = 0;
int ledLevel = 0;

// Switch LED "breathing" effect: fades up and down once per period.
const unsigned long LED_PULSE_PERIOD_MS = 3000;
bool ledPulse = false;

int beepsLeft = 0;            // beeps still to play
bool beepSounding = false;
unsigned long beepNextAt = 0;

// 1 = power on/off, 2 = reset, 3 = forced power off
void startBeeps(int n) {
#if BUZZER_OK
  beepsLeft = constrain(n, 1, 3);
  beepSounding = false;
  beepNextAt = millis();
#endif
}

void updateBeeps() {
  if (beepsLeft == 0 && !beepSounding) return;
  unsigned long now = millis();
  if ((long)(now - beepNextAt) < 0) return;
  if (beepSounding) {
    digitalWrite(PIN_BUZZER, LOW);
    beepSounding = false;
    beepNextAt = now + BEEP_GAP_MS;
  } else if (beepsLeft > 0) {
    digitalWrite(PIN_BUZZER, HIGH);
    beepSounding = true;
    beepsLeft--;
    beepNextAt = now + BEEP_ON_MS;
  }
}

// ---- Bluetooth controller detection (test only) -----------------------
// Listens for the controller's advertising. It never switches PS_ON or
// presses a button: it only beeps once and counts a "wake event" each
// time the controller appears after being away.
const char CTRL_ADDR[] = "aa:bb:cc:dd:ee:ff";  // put your controller's address here
const unsigned long CTRL_GONE_MS = 5000;       // silent this long = gone
const uint32_t BLE_SCAN_CHUNK_S = 5;           // scan restarts this often

volatile unsigned long ctrlLastSeenAt = 0;
volatile bool ctrlEverSeen = false;
volatile int ctrlRssi = 0;
volatile unsigned long bleAdverts = 0;         // adverts from any device
volatile bool bleScanDone = false;
bool ctrlPresent = false;
unsigned long ctrlWakeCount = 0;
BLEScan *bleScan = nullptr;

class CtrlScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    bleAdverts = bleAdverts + 1;
    if (dev.getAddress().toString().equalsIgnoreCase(CTRL_ADDR)) {
      ctrlRssi = dev.getRSSI();
      ctrlLastSeenAt = millis();
      ctrlEverSeen = true;
    }
  }
};

void bleScanComplete(BLEScanResults results) { bleScanDone = true; }

void startBleScan() {
  bleScan->clearResults();   // the library keeps a list of devices; drop it
  bleScanDone = false;
  bleScan->start(BLE_SCAN_CHUNK_S, bleScanComplete, false);
}

void updateController() {
  if (bleScanDone) startBleScan();
  bool present = ctrlEverSeen && (millis() - ctrlLastSeenAt < CTRL_GONE_MS);
  if (present && !ctrlPresent) {
    ctrlWakeCount++;
    startBeeps(1);
  }
  ctrlPresent = present;
}

void updateLedPulse() {
  if (!ledPulse) return;
  static unsigned long lastAt = 0;
  unsigned long now = millis();
  if (now - lastAt < 20) return;   // 50 updates a second is smooth enough
  lastAt = now;
  float phase = (now % LED_PULSE_PERIOD_MS) / (float)LED_PULSE_PERIOD_MS;
  float v = (1.0f - cosf(phase * 2.0f * PI)) / 2.0f;  // 0..1..0
  v = v * v;                       // rough gamma so the fade looks even
  analogWrite(PIN_LED, (int)(v * 255.0f + 0.5f));
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BC-250 test</title>
<style>
body{font-family:system-ui,sans-serif;margin:16px;max-width:480px}
h1{font-size:20px} h2{font-size:15px;margin:22px 0 8px}
button{font-size:16px;padding:12px 14px;margin:4px 6px 4px 0;border-radius:8px;border:1px solid #888;background:#f2f2f2}
.row{display:flex;justify-content:space-between;padding:10px 12px;border:1px solid #ccc;border-radius:8px;margin:6px 0}
.on{background:#c9efc9}.off{background:#eee}
input[type=range]{width:100%}
small{color:#666}
</style></head><body>
<h1>BC-250 controller test</h1>

<h2>Outputs (optos)</h2>
<button onclick="go('/pson')">PS_ON toggle</button>
<button onclick="go('/pwr')">PWR pulse</button>
<button onclick="go('/rst')">RST pulse</button>
<div class="row" id="rp">PS_ON drive (D1)<b id="pson">-</b></div>
<div class="row" id="rw">PWR drive (D4)<b id="pwr">-</b></div>
<div class="row" id="rr">RST drive (D5)<b id="rst">-</b></div>

<h2>Inputs</h2>
<div class="row" id="rs">BC-250 LED sense (D3)<b id="sense">-</b></div>
<div class="row" id="rb">Front switch (D7)<b id="sw">-</b></div>

<h2>Bluetooth controller</h2>
<div class="row" id="rc">8BitDo controller<b id="ctrl">-</b></div>
<div class="row">Wake events<b id="wakes">-</b></div>
<div class="row">Adverts heard (any device)<b id="adv">-</b></div>

<h2>Switch LED (D10)</h2>
<input type="range" min="0" max="255" value="0" oninput="go('/led?v='+this.value)">
<button onclick="go('/pulse')">Pulse on / off</button>
<div class="row" id="rl">Pulse effect<b id="pulse">-</b></div>

<h2>Buzzer (D2)</h2>
<button onclick="go('/beep?n=1')">1 beep</button>
<button onclick="go('/beep?n=2')">2 beeps</button>
<button onclick="go('/beep?n=3')">3 beeps</button>
<br><small id="bz"></small>

<script>
var go=function(u){fetch(u).then(poll)};
var set=function(id,row,on,t,f){document.getElementById(id).textContent=on?t:f;
  document.getElementById(row).className='row '+(on?'on':'off')};
var poll=function(){fetch('/state').then(r=>r.json()).then(s=>{
  set('pson','rp',s.pson,'HIGH (on)','low');
  set('pwr','rw',s.pwr,'HIGH (pressing)','low');
  set('rst','rr',s.rst,'HIGH (pressing)','low');
  set('sense','rs',s.sense,'LED lit','dark');
  set('sw','rb',s.sw,'pressed','released');
  set('pulse','rl',s.pulse,'pulsing','off');
  set('ctrl','rc',s.ctrl,'seen, '+s.rssi+' dBm','not seen');
  document.getElementById('wakes').textContent=s.wakes;
  document.getElementById('adv').textContent=s.adv;
  document.getElementById('bz').textContent=s.buzzer?'':'Buzzer disabled in firmware (BUZZER_OK is 0).';
})};
setInterval(poll,400);poll();
</script></body></html>)HTML";

void sendOk() { server.send(200, "text/plain", "ok"); }

void handleState() {
  String j = "{";
  j += "\"pson\":"  + String(psOn ? 1 : 0);
  j += ",\"pwr\":"  + String(digitalRead(PIN_PWR));
  j += ",\"rst\":"  + String(digitalRead(PIN_RST));
  j += ",\"sense\":" + String(digitalRead(PIN_SENSE) == LOW ? 1 : 0);
  j += ",\"sw\":"   + String(digitalRead(PIN_SWITCH) == LOW ? 1 : 0);
  j += ",\"led\":"  + String(ledLevel);
  j += ",\"pulse\":" + String(ledPulse ? 1 : 0);
  j += ",\"buzzer\":" + String(BUZZER_OK);
  j += ",\"ctrl\":" + String(ctrlPresent ? 1 : 0);
  j += ",\"rssi\":" + String((int)ctrlRssi);
  j += ",\"wakes\":" + String(ctrlWakeCount);
  j += ",\"adv\":" + String((unsigned long)bleAdverts);
  j += "}";
  server.send(200, "application/json", j);
}

void setup() {
  // Outputs first, all LOW, so no opto is on while the rest starts.
  pinMode(PIN_PSON, OUTPUT);   digitalWrite(PIN_PSON, LOW);
  pinMode(PIN_PWR, OUTPUT);    digitalWrite(PIN_PWR, LOW);
  pinMode(PIN_RST, OUTPUT);    digitalWrite(PIN_RST, LOW);
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  // The buzzer hangs straight on the pin; default drive (about 20 mA) is
  // too weak for it. Use the strongest setting (about 40 mA).
  gpio_set_drive_capability((gpio_num_t)PIN_BUZZER, GPIO_DRIVE_CAP_3);
  pinMode(PIN_LED, OUTPUT);    digitalWrite(PIN_LED, LOW);

  pinMode(PIN_SENSE, INPUT_PULLUP);
  pinMode(PIN_SWITCH, INPUT_PULLUP);

  // Serial is only used when it is the USB port. If "USB CDC On Boot" is
  // off, Serial would be UART0, whose RX pin is GPIO20 = the switch input.
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.begin(115200);
#endif

  WiFi.mode(WIFI_AP);
  WiFi.softAP("BC250-Test", "bc250test");

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/state", handleState);

  server.on("/pson", []() {
    psOn = !psOn;
    digitalWrite(PIN_PSON, psOn ? HIGH : LOW);
    sendOk();
  });
  server.on("/pwr", []() {
    digitalWrite(PIN_PWR, HIGH);
    pwrOffAt = millis() + PULSE_MS;
    sendOk();
  });
  server.on("/rst", []() {
    digitalWrite(PIN_RST, HIGH);
    rstOffAt = millis() + PULSE_MS;
    sendOk();
  });
  server.on("/led", []() {
    ledLevel = constrain(server.arg("v").toInt(), 0, 255);
    ledPulse = false;              // moving the slider stops the pulse
    analogWrite(PIN_LED, ledLevel);
    sendOk();
  });
  server.on("/pulse", []() {
    ledPulse = !ledPulse;
    if (!ledPulse) analogWrite(PIN_LED, ledLevel);  // back to the slider level
    sendOk();
  });
  server.on("/beep", []() {
    startBeeps(server.arg("n").toInt());
    sendOk();
  });

  server.begin();

  // Passive scan, listening 60 ms out of every 200 ms so the WiFi access
  // point (same radio) keeps working.
  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new CtrlScanCallbacks(), true);
  bleScan->setActiveScan(false);
  bleScan->setInterval(200);
  bleScan->setWindow(60);
  startBleScan();
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.println("Test page: http://192.168.4.1");
#endif
}

void loop() {
  server.handleClient();
  updateBeeps();
  updateLedPulse();
  updateController();

  unsigned long now = millis();
  if (pwrOffAt && (long)(now - pwrOffAt) >= 0) {
    digitalWrite(PIN_PWR, LOW);
    pwrOffAt = 0;
  }
  if (rstOffAt && (long)(now - rstOffAt) >= 0) {
    digitalWrite(PIN_RST, LOW);
    rstOffAt = 0;
  }
}
