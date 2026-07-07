
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESPAsyncWebServer.h>

// ---------------------------------------------------------------------------
// Project: WiFi Controlled Electric Wheelchair
// ---------------------------------------------------------------------------
static const char *PROJECT_NAME = "WiFi Controlled Electric Wheelchair";

// ---------------------------------------------------------------------------
// Wi-Fi credentials
// ---------------------------------------------------------------------------
static const char *WIFI_SSID = "junior";
static const char *WIFI_PASSWORD = "junior123";
static const char *MDNS_HOSTNAME = "wheelchair"; // open http://wheelchair.local

// ---------------------------------------------------------------------------
// LCD 20x4 (I2C). Change the address to 0x3F if 0x27 shows a blank screen.
// 21/22 are the standard I2C pins on classic ESP32 dev boards.
// ---------------------------------------------------------------------------
#define LCD_I2C_ADDRESS 0x27
#define LCD_SDA_PIN 21
#define LCD_SCL_PIN 22
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 20, 4);

// ---------------------------------------------------------------------------
// Status LEDs
//  - LED_WIFI_PIN   : OFF while searching for WiFi, blinking while trying to
//                     connect, solid ON once connected.
//  - LED_POWER_PIN  : solid ON once the board has booted and the LCD (I2C)
//                     is up and running - a simple "system alive" indicator.
// ---------------------------------------------------------------------------
#define LED_WIFI_PIN 4
#define LED_POWER_PIN 5
#define LED_WIFI_BLINK_MS 400

// ---------------------------------------------------------------------------
// Motor control: single relay switching the high-current motor circuit
// on/off (no H-bridge / no PWM speed or direction control).
//   - Relay coil control input -> RELAY_PIN
//   - Relay COM/NO contacts wired in series with the motor's battery feed
//   - RELAY_ACTIVE_LOW: most cheap 1-channel relay modules energize the
//     relay (turn the motor ON) when the input pin is driven LOW. If your
//     motor turns ON when it should be OFF (and vice versa), flip this.
// ---------------------------------------------------------------------------
#define RELAY_PIN 26
#define RELAY_ACTIVE_LOW false

static inline void relayWrite(bool on) {
	digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_LOW) ? LOW : HIGH);
}

// If no movement command is received for this long while moving, stop.
#define COMMAND_TIMEOUT_MS 400
// LCD is refreshed at most this often so it never blocks motor control.
#define LCD_REFRESH_MS 250

// ---------------------------------------------------------------------------
enum Direction { STOPPED,
				 FORWARD,
				 LEFT,
				 RIGHT };

Direction currentDirection = STOPPED;
unsigned long lastCommandMillis = 0;
unsigned long lastLcdUpdateMillis = 0;
unsigned long lastWifiBlinkMillis = 0;
bool wifiLedState = false;
bool wifiConnected = false;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ---------------------------------------------------------------------------
// Motor control
// ---------------------------------------------------------------------------
void stopMotors() {
	relayWrite(false);
}

// TEMPORARY (single-motor stage): the relay only switches the motor
// current on/off, so there is no reverse and no independent left/right
// drive. FORWARD/LEFT/RIGHT all just close the relay (motor ON) - they
// keep separate names for LCD/web display, but move the chair
// identically. BACK is not wired to the motor at all; it just reports
// that reverse is blocked. Revisit once a real direction-capable driver
// is in place.
void driveForward() {
	Serial.println("[DEBUG] driveForward() called");
	relayWrite(true);
	Serial.println("[DEBUG] relay ON");
}

const char *directionName(Direction d) {
	switch (d) {
		case FORWARD: return "FORWARD";
		case LEFT: return "LEFT";
		case RIGHT: return "RIGHT";
		default: return "STOPPED";
	}
}

void setDirection(Direction d) {
	if (d != currentDirection) {
		currentDirection = d;
		switch (d) {
			case FORWARD:
			case LEFT:
			case RIGHT:
				driveForward();
				break;
			default:
				stopMotors();
				break;
		}
		ws.textAll(directionName(currentDirection));
	}
	lastCommandMillis = millis();
}

// ---------------------------------------------------------------------------
// LCD status - the title (row 0) and IP address (row 1) are considered
// permanent: once WiFi connects, the IP is written once and is never
// cleared or overwritten by any other status update.
// ---------------------------------------------------------------------------
void updateLcd(bool force = false) {
	unsigned long now = millis();
	if (!force && now - lastLcdUpdateMillis < LCD_REFRESH_MS) return;
	lastLcdUpdateMillis = now;

	lcd.setCursor(0, 0);
	lcd.print("WIFI WHEELCHAIR CTRL");

	lcd.setCursor(0, 1);
	if (wifiConnected) {
		String line = "IP:" + WiFi.localIP().toString();
		while (line.length() < 20) line += ' ';
		lcd.print(line);
	} else {
		lcd.print("WiFi: connecting...  ");
	}

	lcd.setCursor(0, 2);
	{
		String line = String("Dir: ") + directionName(currentDirection);
		while (line.length() < 20) line += ' ';
		lcd.print(line);
	}

	lcd.setCursor(0, 3);
	{
		String line = String("WiFi:") + (wifiConnected ? "OK" : "NO") + " Clients:" + String(ws.count());
		while (line.length() < 20) line += ' ';
		lcd.print(line);
	}
}

// ---------------------------------------------------------------------------
// Status LEDs
// ---------------------------------------------------------------------------
void updateWifiLed() {
	if (wifiConnected) {
		digitalWrite(LED_WIFI_PIN, HIGH);
		return;
	}
	unsigned long now = millis();
	if (now - lastWifiBlinkMillis >= LED_WIFI_BLINK_MS) {
		lastWifiBlinkMillis = now;
		wifiLedState = !wifiLedState;
		digitalWrite(LED_WIFI_PIN, wifiLedState ? HIGH : LOW);
	}
}

// ---------------------------------------------------------------------------
// Web page (self-contained, no internet/CDN needed).
// Responsive layout: scales cleanly from small phones up to tablets/desktop
// browsers using viewport units + clamp(), and respects notch safe areas.
// ---------------------------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
<title>WiFi Controlled Electric Wheelchair</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  html, body { height: 100%; }
  body {
    margin: 0; min-height: 100vh; display: flex; flex-direction: column;
    align-items: center; justify-content: center; gap: clamp(12px, 3vh, 24px);
    padding: env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left);
    background: radial-gradient(circle at 50% -10%, #182233 0%, #0b0e13 60%);
    color: #eaeef2;
    font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
    user-select: none; touch-action: manipulation;
  }
  .card {
    width: min(94vw, 460px);
    display: flex; flex-direction: column; align-items: center;
    gap: clamp(10px, 2.4vh, 20px);
    padding: clamp(16px, 3vh, 28px) clamp(14px, 4vw, 28px);
    background: #131822; border: 1px solid #232c3a; border-radius: 22px;
    box-shadow: 0 16px 40px rgba(0,0,0,.45);
  }
  .brand { display: flex; flex-direction: column; align-items: center; gap: 4px; }
  .brand .logo { font-size: clamp(1.6rem, 6vw, 2.1rem); }
  h1 {
    font-size: clamp(.95rem, 3.4vw, 1.25rem); margin: 0; text-align: center;
    color: #8fb8ff; letter-spacing: .04em; font-weight: 700;
  }
  .subtitle { font-size: clamp(.65rem, 2.4vw, .78rem); color: #6b7684; letter-spacing: .12em; text-transform: uppercase; }
  #status {
    padding: 6px 16px; border-radius: 999px; font-weight: 600; font-size: clamp(.75rem, 2.6vw, .9rem);
    background: #3a1414; color: #ff8a8a; border: 1px solid #6b1f1f;
    display: flex; align-items: center; gap: 8px;
  }
  #status::before { content: ""; width: 8px; height: 8px; border-radius: 50%; background: currentColor; }
  #status.connected { background: #123a1c; color: #7be08c; border-color: #1f6b34; }
  #action {
    font-size: clamp(1.2rem, 5vw, 1.7rem); font-weight: 800; letter-spacing: .1em;
    min-height: 2.2rem; color: #fff;
  }
  .pad {
    display: grid;
    grid-template-columns: repeat(3, minmax(72px, 110px));
    grid-template-rows: repeat(3, minmax(72px, 110px));
    gap: clamp(10px, 2.6vw, 16px);
    width: 100%;
    justify-content: center;
  }
  button {
    border: none; border-radius: 18px; font-size: clamp(.85rem, 3vw, 1.05rem); font-weight: 700;
    color: #fff; background: #1c2530; box-shadow: 0 4px 0 #0a0d11;
    cursor: pointer; display: flex; align-items: center; justify-content: center;
    gap: 6px; transition: transform .05s ease;
  }
  button:active { transform: translateY(3px); box-shadow: none; }
  #btnForward { grid-column: 2; grid-row: 1; background: #1e5fa8; }
  #btnLeft    { grid-column: 1; grid-row: 2; background: #1e5fa8; }
  #btnRight   { grid-column: 3; grid-row: 2; background: #1e5fa8; }
  #btnStop    { grid-column: 2; grid-row: 2; background: #a81e1e; }
  #btnBack    { grid-column: 2; grid-row: 3; background: #7a3a12; }
  button:disabled { opacity: .35; box-shadow: none; }
  #hint { font-size: clamp(.6rem, 2.2vw, .75rem); color: #6b7684; max-width: 280px; text-align: center; }
  footer { font-size: .65rem; color: #414b59; letter-spacing: .06em; }
  .clock {
    display: flex; flex-direction: column; align-items: center; gap: 2px;
    padding: 6px 18px; border-radius: 14px; background: #0e1420; border: 1px solid #202939;
  }
  .clock .time { font-size: clamp(1.1rem, 4vw, 1.5rem); font-weight: 700; color: #eaeef2; font-variant-numeric: tabular-nums; }
  .clock .date { font-size: clamp(.65rem, 2.2vw, .78rem); color: #7a8697; }
  .voice-wrap { display: flex; flex-direction: column; align-items: center; gap: 6px; width: 100%; }
  #btnVoice {
    width: 100%; max-width: 260px; height: 48px; border-radius: 14px;
    background: #23324a; box-shadow: 0 4px 0 #0a0d11; font-size: clamp(.8rem, 2.8vw, .95rem);
  }
  #btnVoice.listening { background: #1f6b34; animation: pulse 1.2s ease-in-out infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: .7; } }
  #voiceStatus { font-size: clamp(.6rem, 2.2vw, .75rem); color: #6b7684; min-height: 1em; text-align: center; }

  @media (min-width: 720px) {
    .card { width: 460px; }
  }
</style>
</head>
<body>
  <div class="card">
    <div class="brand">
      <div class="logo">&#9877;&#65039;</div>
      <h1>WIFI CONTROLLED<br>ELECTRIC WHEELCHAIR</h1>
      <div class="subtitle">Remote Control Panel</div>
    </div>
    <div class="clock">
      <div class="time" id="clockTime">--:--:--</div>
      <div class="date" id="clockDate">-- --- ----</div>
    </div>
    <div id="status">Connecting...</div>
    <div id="action">STOPPED</div>
    <div class="pad">
      <button id="btnForward">&#9650; FWD</button>
      <button id="btnLeft">&#9664; LEFT</button>
      <button id="btnStop">STOP</button>
      <button id="btnRight">&#9654; RIGHT</button>
      <button id="btnBack">&#9660; BACK</button>
    </div>
    <div class="voice-wrap">
      <button id="btnVoice">&#127908; Amri kwa Sauti (Voice)</button>
      <div id="voiceStatus">Sema: "mbele", "kushoto", "kulia", "nyuma", "simama"</div>
    </div>
    <div id="hint">Press and hold a direction to move. Release to stop.</div>
    <footer>Wheelchair Control System</footer>
  </div>

<script>
const statusEl = document.getElementById('status');
const actionEl = document.getElementById('action');
const buttons = [...document.querySelectorAll('.pad button')];
let ws = null;
let holdTimer = null;

function setConnected(ok) {
  statusEl.textContent = ok ? 'Connected' : 'Disconnected';
  statusEl.className = ok ? 'connected' : '';
  buttons.forEach(b => b.disabled = !ok);
  if (!ok) actionEl.textContent = 'STOPPED';
}

function send(cmd) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(cmd);
}

function startMoving(cmd) {
  send(cmd);
  clearInterval(holdTimer);
  holdTimer = setInterval(() => send(cmd), 150);
}

function forceStop() {
  clearInterval(holdTimer);
  holdTimer = null;
  send('S');
}

function bindHold(el, cmd) {
  const start = (e) => {
    e.preventDefault();
    startMoving(cmd);
  };
  el.addEventListener('pointerdown', start);
  el.addEventListener('pointerup', forceStop);
  el.addEventListener('pointerleave', forceStop);
  el.addEventListener('pointercancel', forceStop);
}

bindHold(document.getElementById('btnForward'), 'F');
bindHold(document.getElementById('btnLeft'), 'L');
bindHold(document.getElementById('btnRight'), 'R');
document.getElementById('btnStop').addEventListener('pointerdown', forceStop);
document.getElementById('btnBack').addEventListener('pointerdown', (e) => { e.preventDefault(); send('B'); });

document.addEventListener('visibilitychange', () => { if (document.hidden) forceStop(); });
window.addEventListener('blur', forceStop);

const BACK_BLOCKED_MSG = 'Marufuku! Wheelchair haiwezi kwenda nyuma. Angalia manual move.';

function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = () => setConnected(true);
  ws.onclose = () => { setConnected(false); setTimeout(connect, 1000); };
  ws.onerror = () => ws.close();
  ws.onmessage = (evt) => {
    if (evt.data === 'BACK_BLOCKED') {
      actionEl.textContent = 'NYUMA: MARUFUKU';
      speak(BACK_BLOCKED_MSG);
      return;
    }
    actionEl.textContent = evt.data;
    speakSwahili(evt.data);
  };
}
connect();

// ---------------------------------------------------------------------------
// Live clock (device local time/date) - updates every second.
// ---------------------------------------------------------------------------
const clockTimeEl = document.getElementById('clockTime');
const clockDateEl = document.getElementById('clockDate');
function tickClock() {
  const now = new Date();
  clockTimeEl.textContent = now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  clockDateEl.textContent = now.toLocaleDateString([], { weekday: 'short', year: 'numeric', month: 'short', day: '2-digit' });
}
tickClock();
setInterval(tickClock, 1000);

// ---------------------------------------------------------------------------
// Swahili voice control + spoken feedback.
// ---------------------------------------------------------------------------
const SWAHILI_WORDS = {
  STOPPED: 'Simama', FORWARD: 'Mbele', LEFT: 'Kushoto', RIGHT: 'Kulia'
};
function speak(text) {
  if (!('speechSynthesis' in window)) return;
  const utter = new SpeechSynthesisUtterance(text);
  utter.lang = 'sw-KE';
  speechSynthesis.cancel();
  speechSynthesis.speak(utter);
}
function speakSwahili(actionWord) {
  speak(SWAHILI_WORDS[actionWord] || actionWord);
}

const btnVoice = document.getElementById('btnVoice');
const voiceStatusEl = document.getElementById('voiceStatus');
const SpeechRecognitionImpl = window.SpeechRecognition || window.webkitSpeechRecognition;
let recognition = null;
let voiceOn = false;

if (!SpeechRecognitionImpl) {
  voiceStatusEl.textContent = 'Voice control haipatikani kwenye kivinjari hiki (browser not supported)';
  btnVoice.disabled = true;
} else {
  recognition = new SpeechRecognitionImpl();
  recognition.lang = 'sw-TZ';
  recognition.continuous = true;
  recognition.interimResults = false;

  recognition.onresult = (event) => {
    const last = event.results[event.results.length - 1];
    const transcript = last[0].transcript.toLowerCase();
    if (transcript.includes('mbele')) startMoving('F');
    else if (transcript.includes('kushoto')) startMoving('L');
    else if (transcript.includes('kulia')) startMoving('R');
    else if (transcript.includes('nyuma') || transcript.includes('rudi')) send('B');
    else if (transcript.includes('simama') || transcript.includes('acha')) forceStop();
  };
  recognition.onerror = () => { /* keep listening; onend will attempt restart */ };
  recognition.onend = () => { if (voiceOn) { try { recognition.start(); } catch (e) {} } };

  btnVoice.addEventListener('click', () => {
    voiceOn = !voiceOn;
    btnVoice.classList.toggle('listening', voiceOn);
    if (voiceOn) {
      voiceStatusEl.textContent = 'Sikiliza... sema "mbele", "kushoto", "kulia", "nyuma", "simama"';
      try { recognition.start(); } catch (e) {}
    } else {
      voiceStatusEl.textContent = 'Sema: "mbele", "kushoto", "kulia", "nyuma", "simama"';
      recognition.stop();
      forceStop();
    }
  });
}
</script>
</body>
</html>
)HTML";

// ---------------------------------------------------------------------------
// WebSocket handling
// ---------------------------------------------------------------------------
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
	switch (type) {
		case WS_EVT_CONNECT:
			Serial.printf("WS client #%u connected\n", client->id());
			setDirection(STOPPED);
			updateLcd(true);
			break;
		case WS_EVT_DISCONNECT:
			Serial.printf("WS client #%u disconnected\n", client->id());
			setDirection(STOPPED); // safety: stop the moment control is lost
			updateLcd(true);
			break;
		case WS_EVT_DATA: {
			if (len < 1) break;
			char cmd = (char)data[0];
			Serial.printf("[DEBUG] WS cmd received: '%c' (len=%u)\n", cmd, (unsigned)len);
			switch (cmd) {
				case 'F': setDirection(FORWARD); break;
				case 'L': setDirection(LEFT); break;
				case 'R': setDirection(RIGHT); break;
				case 'S': setDirection(STOPPED); break;
				case 'B':
					// Reverse is not available on this hardware - refuse and
					// warn instead of touching the motor.
					Serial.println("[DEBUG] BACK requested - blocked (no reverse)");
					client->text("BACK_BLOCKED");
					break;
				default: break;
			}
			break;
		}
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
void setup() {
	Serial.begin(115200);

	pinMode(RELAY_PIN, OUTPUT);
	stopMotors(); // motor must start stopped

	pinMode(LED_WIFI_PIN, OUTPUT);
	pinMode(LED_POWER_PIN, OUTPUT);
	digitalWrite(LED_WIFI_PIN, LOW);
	digitalWrite(LED_POWER_PIN, LOW);

	Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
	lcd.init();
	lcd.backlight();
	lcd.clear();
	digitalWrite(LED_POWER_PIN, HIGH); // system + LCD (I2C) alive
	updateLcd(true);

	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
	Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);

	unsigned long wifiStart = millis();
	while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
		updateWifiLed();
		delay(50);
		Serial.print(".");
	}

	if (WiFi.status() == WL_CONNECTED) {
		wifiConnected = true;
		digitalWrite(LED_WIFI_PIN, HIGH);
		Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
		if (MDNS.begin(MDNS_HOSTNAME)) {
			Serial.printf("mDNS ready: http://%s.local\n", MDNS_HOSTNAME);
		}
	} else {
		Serial.println("\nWiFi FAILED to connect - check SSID/password.");
	}
	updateLcd(true);

	ws.onEvent(onWsEvent);
	server.addHandler(&ws);

	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send_P(200, "text/html", INDEX_HTML);
	});

	server.begin();
	Serial.printf("%s ready.\n", PROJECT_NAME);
}

void loop() {
	ws.cleanupClients();

	// Safety watchdog: no fresh command while moving -> stop.
	if (currentDirection != STOPPED && millis() - lastCommandMillis > COMMAND_TIMEOUT_MS) {
		Serial.println("Command timeout - stopping motors.");
		setDirection(STOPPED);
	}

	updateWifiLed();
	updateLcd();
}
