#include <Arduino.h>
#include <TM1637Display.h>
#include <ESP32Encoder.h>
#include <OneButton.h>

// ---- Pinos ----
#define PIN_TM_CLK  21
#define PIN_TM_DIO  19
#define PIN_ENC_A   18
#define PIN_ENC_B    5
#define PIN_ENC_BTN 17
#define PIN_BUZZER  16
const uint8_t LED_PINS[6] = {32, 33, 25, 26, 27, 14};

// ---- BPM ----
#define BPM_MIN      40
#define BPM_MAX     300
#define BPM_DEFAULT 120

// ---- Buzzer ----
#define BUZZ_FREQ_DOWN 1000
#define BUZZ_FREQ_BEAT  600
#define BUZZ_DUR_DOWN    60
#define BUZZ_DUR_BEAT    40
#define LEDC_CHANNEL      0

// ---- Timer (prescaler 80 → 1MHz tick) ----
#define TIMER_NUM        0
#define TIMER_PRESCALER 80

// ---- Preview TIME_SIG a 60 BPM fixo ----
#define PREVIEW_INTERVAL_MS 1000UL

// ---- Tipos ----
enum State : uint8_t { IDLE, RUNNING, TIME_SIG };

struct TimeSig { uint8_t num; uint8_t den; };
const TimeSig TIME_SIGS[] = {{2,4},{3,4},{4,4},{5,4},{6,8}};
const uint8_t NUM_SIGS = sizeof(TIME_SIGS) / sizeof(TIME_SIGS[0]);

// ---- Estado global ----
State     appState   = IDLE;
bool      wasRunning = false;
int       bpm        = BPM_DEFAULT;
uint8_t   beatIndex  = 0;
uint8_t   sigIdx     = 2;  // 4/4

volatile bool beatFlag = false;

hw_timer_t   *beatTimer    = nullptr;
unsigned long buzzerOffTime = 0;
bool          buzzerActive  = false;
unsigned long previewLastMs = 0;
uint8_t       previewBeat   = 0;
long          lastEncPos    = 0;

// ---- Objetos ----
TM1637Display display(PIN_TM_CLK, PIN_TM_DIO);
ESP32Encoder  encoder;
OneButton     button(PIN_ENC_BTN, true);  // active LOW, pull-up interno

// ---- Declarações ----
void IRAM_ATTR onBeatISR();
uint8_t rotateSegment(uint8_t s);
void showFlipped(const uint8_t segs[4]);
void showNumberFlipped(int n);
void updateDisplay();
void allLedsOff();
void startBeat(uint8_t idx);
void handleBeat();
void rearmTimer();
void onEncoderChange(int delta);
void onShortPress();
void onLongPress();

// ================================================================
void setup() {
    setCpuFrequencyMhz(80);  // 240 → 80 MHz; APB permanece 80 MHz, timer inalterado
    Serial.end();            // UART desligada para economizar energia

    for (uint8_t i = 0; i < 6; i++) {
        pinMode(LED_PINS[i], OUTPUT);
        digitalWrite(LED_PINS[i], LOW);
    }

    ledcSetup(LEDC_CHANNEL, BUZZ_FREQ_DOWN, 8);
    ledcAttachPin(PIN_BUZZER, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 0);

    display.setBrightness(1);
    updateDisplay();

    ESP32Encoder::useInternalWeakPullResistors = UP;
    encoder.attachHalfQuad(PIN_ENC_A, PIN_ENC_B);
    encoder.setCount(0);
    lastEncPos = 0;

    button.setDebounceMs(20);
    button.setClickMs(200);
    button.setPressMs(600);
    button.attachClick(onShortPress);
    button.attachLongPressStart(onLongPress);

    beatTimer = timerBegin(TIMER_NUM, TIMER_PRESCALER, true);
    timerAttachInterrupt(beatTimer, &onBeatISR, true);
}

// ================================================================
void loop() {
    button.tick();

    long pos   = encoder.getCount();
    long delta = pos - lastEncPos;
    if (delta != 0) {
        lastEncPos = pos;
        onEncoderChange((int)delta);
    }

    if (beatFlag) {
        beatFlag = false;
        if (appState == RUNNING) handleBeat();
    }

    if (appState == TIME_SIG) {
        unsigned long now = millis();
        if (now - previewLastMs >= PREVIEW_INTERVAL_MS) {
            previewLastMs = now;
            allLedsOff();
            digitalWrite(LED_PINS[previewBeat % TIME_SIGS[sigIdx].num], HIGH);
            previewBeat++;
        }
    }

    if (buzzerActive && millis() >= buzzerOffTime) {
        ledcWrite(LEDC_CHANNEL, 0);
        buzzerActive = false;
    }
}

// ================================================================
void IRAM_ATTR onBeatISR() {
    beatFlag = true;
}

// Rotaciona segmentos 7-seg 180°: a↔d, b↔e, c↔f, g permanece
uint8_t rotateSegment(uint8_t s) {
    uint8_t r = s & 0x40;
    if (s & 0x01) r |= 0x08;  // a → d
    if (s & 0x08) r |= 0x01;  // d → a
    if (s & 0x02) r |= 0x10;  // b → e
    if (s & 0x10) r |= 0x02;  // e → b
    if (s & 0x04) r |= 0x20;  // c → f
    if (s & 0x20) r |= 0x04;  // f → c
    return r;
}

// Exibe 4 segmentos com display invertido (ordem revertida + cada dígito rotacionado)
void showFlipped(const uint8_t segs[4]) {
    uint8_t f[4];
    for (uint8_t i = 0; i < 4; i++) f[i] = rotateSegment(segs[3 - i]);
    display.setSegments(f);
}

// Exibe número inteiro (0–9999) com zeros à esquerda no display invertido
void showNumberFlipped(int n) {
    uint8_t segs[4];
    for (int i = 3; i >= 0; i--) { segs[i] = display.encodeDigit(n % 10); n /= 10; }
    showFlipped(segs);
}

void allLedsOff() {
    for (uint8_t i = 0; i < 6; i++) digitalWrite(LED_PINS[i], LOW);
}

void rearmTimer() {
    uint64_t alarm = 60000000ULL / (uint64_t)bpm;
    timerAlarmWrite(beatTimer, alarm, true);
    timerWrite(beatTimer, 0);
    timerAlarmEnable(beatTimer);
}

void startBeat(uint8_t idx) {
    allLedsOff();
    digitalWrite(LED_PINS[idx % 6], HIGH);
    bool down = (idx == 0);
    ledcWriteTone(LEDC_CHANNEL, down ? BUZZ_FREQ_DOWN : BUZZ_FREQ_BEAT);
    buzzerOffTime = millis() + (down ? BUZZ_DUR_DOWN : BUZZ_DUR_BEAT);
    buzzerActive = true;
}

void handleBeat() {
    startBeat(beatIndex);
    beatIndex = (beatIndex + 1) % TIME_SIGS[sigIdx].num;
}

void updateDisplay() {
    if (appState == TIME_SIG) {
        uint8_t segs[4] = {
            display.encodeDigit(TIME_SIGS[sigIdx].num),
            0x40,  // '-'
            0x00,  // ' '
            display.encodeDigit(TIME_SIGS[sigIdx].den)
        };
        showFlipped(segs);
    } else {
        showNumberFlipped(bpm);
    }
}

void onEncoderChange(int delta) {
    if (appState == TIME_SIG) {
        int idx = (int)sigIdx + delta;
        if (idx < 0) idx = NUM_SIGS - 1;
        if (idx >= (int)NUM_SIGS) idx = 0;
        sigIdx = (uint8_t)idx;
        previewBeat   = 0;
        previewLastMs = 0;
    } else {
        bpm = constrain(bpm + delta, BPM_MIN, BPM_MAX);
        if (appState == RUNNING) rearmTimer();
    }
    updateDisplay();
}

void onShortPress() {
    if (appState == TIME_SIG) {
        appState = wasRunning ? RUNNING : IDLE;
        allLedsOff();
        beatIndex = 0;
        if (appState == RUNNING) {
            rearmTimer();
            handleBeat();
        }
        updateDisplay();
        return;
    }

    if (appState == IDLE) {
        appState  = RUNNING;
        beatIndex = 0;
        rearmTimer();
        handleBeat();
    } else {
        appState = IDLE;
        timerAlarmDisable(beatTimer);
        allLedsOff();
        ledcWrite(LEDC_CHANNEL, 0);
        buzzerActive = false;
    }
    updateDisplay();
}

void onLongPress() {
    wasRunning = (appState == RUNNING);
    if (appState == RUNNING) {
        timerAlarmDisable(beatTimer);
        ledcWrite(LEDC_CHANNEL, 0);
        buzzerActive = false;
    }
    allLedsOff();
    beatIndex     = 0;
    previewBeat   = 0;
    previewLastMs = 0;
    appState      = TIME_SIG;
    updateDisplay();
}
