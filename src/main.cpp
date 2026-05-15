#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <RotaryEncoder.h>
#include <OneButton.h>
#include <driver/i2s.h>
#include <esp_wifi.h>
#include <math.h>

// ---- Pinos ----
#define PIN_I2S_BCLK  4
#define PIN_I2S_WS    3
#define PIN_I2S_DOUT  1
#define PIN_MAX_SD    0   // SD do MAX98357: LOW = shutdown, HIGH = ativo
#define PIN_SDA       5
#define PIN_SCL       6
#define PIN_ENC_A     7
#define PIN_ENC_B    10
#define PIN_ENC_SW   20

// ---- BPM ----
#define BPM_MIN      40
#define BPM_MAX     300
#define BPM_DEFAULT  60

// ---- Áudio I2S ----
#define SAMPLE_RATE    16000
#define AMPLITUDE      28000   // pico < 32767 para evitar clipping

// Duração dos cliques em amostras (facilita variação futura de sons)
#define DOWN_SAMPLES  480   // downbeat: 1000 Hz × 30 ms
#define BEAT_SAMPLES  320   // beat:      600 Hz × 20 ms

// ---- Timer: prescaler 80 → 1 MHz (1 µs por tick) ----
#define TIMER_PRESCALER 80

// ---- Tipos ----
enum AppState : uint8_t { IDLE, RUNNING };
enum EncMode  : uint8_t { MODE_BPM, MODE_COMPASS, MODE_VOLUME };

struct TimeSig { uint8_t num; uint8_t den; };
const TimeSig TIME_SIGS[] = {{2,4},{3,4},{4,4},{5,4},{6,8}};
const uint8_t NUM_SIGS    = sizeof(TIME_SIGS) / sizeof(TIME_SIGS[0]);

// ---- Estado global ----
AppState appState    = IDLE;
EncMode  encMode     = MODE_BPM;
int      bpm         = BPM_DEFAULT;
uint8_t  sigIdx      = 2;   // 4/4
uint8_t  volume      = 80;  // 0–100
uint8_t  beatIndex   = 0;
uint8_t  displayBeat = 0;   // beat exibido nos indicadores visuais

volatile bool beatFlag = false;

hw_timer_t* beatTimer = nullptr;
long        lastEncPos = 0;

// ---- Buffers de áudio (estéreo 16-bit, L = R para MAX98357 mono) ----
static int16_t downBuf[DOWN_SAMPLES * 2];
static int16_t beatBuf[BEAT_SAMPLES * 2];

// ---- Objetos ----
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
RotaryEncoder encoder(PIN_ENC_A, PIN_ENC_B, RotaryEncoder::LatchMode::TWO03);
OneButton     button(PIN_ENC_SW, true);

// ---- Declarações ----
void IRAM_ATTR onBeatISR();
void IRAM_ATTR checkEncoder();
void rearmTimer();
void generateAudio();
void playAudio(bool isDownbeat);
void handleBeat();
void updateDisplay();
void onEncoderChange(int delta);
void onShortPress();
void onLongPress();

// ================================================================
void setup() {
    setCpuFrequencyMhz(80);

    // Desliga Wi-Fi/BT para economizar energia
    esp_wifi_stop();
    esp_wifi_deinit();

    // MAX98357 começa em shutdown — silêncio garantido antes de qualquer I2S
    pinMode(PIN_MAX_SD, OUTPUT);
    digitalWrite(PIN_MAX_SD, LOW);

    // I2S → MAX98357
    // tx_desc_auto_clear: DMA envia zeros (silêncio digital) quando vazio
    const i2s_config_t i2sCfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 64,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
    };
    i2s_driver_install(I2S_NUM_0, &i2sCfg, 0, NULL);

    const i2s_pin_config_t pinCfg = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = PIN_I2S_BCLK,
        .ws_io_num    = PIN_I2S_WS,
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    i2s_set_pin(I2S_NUM_0, &pinCfg);

    generateAudio();

    // Display OLED
    Wire.begin(PIN_SDA, PIN_SCL);
    u8g2.begin();
    updateDisplay();

    // Encoder (ISR — ESP32-C3 não tem PCNT)
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), checkEncoder, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), checkEncoder, CHANGE);

    // Botão
    button.setDebounceMs(20);
    button.setClickMs(200);
    button.setPressMs(600);
    button.attachClick(onShortPress);
    button.attachLongPressStart(onLongPress);

    // Timer de beat
    beatTimer = timerBegin(0, TIMER_PRESCALER, true);
    timerAttachInterrupt(beatTimer, &onBeatISR, true);
}

// ================================================================
void loop() {
    button.tick();

    long pos   = encoder.getPosition();
    long delta = pos - lastEncPos;
    if (delta != 0) {
        lastEncPos = pos;
        onEncoderChange((int)delta);
    }

    if (beatFlag) {
        beatFlag = false;
        if (appState == RUNNING) handleBeat();
    }
}

// ================================================================
void IRAM_ATTR onBeatISR() {
    beatFlag = true;
}

void IRAM_ATTR checkEncoder() {
    encoder.tick();
}

void rearmTimer() {
    uint64_t ticks = 60000000ULL / (uint64_t)bpm;
    timerAlarmWrite(beatTimer, ticks, true);
    timerWrite(beatTimer, 0);
    timerAlarmEnable(beatTimer);
}

// Gera sine waves com fade-out e volume escalado.
// Chamada no setup e ao alterar o volume.
void generateAudio() {
    const float vol = volume / 100.0f;
    float ph, inc;

    ph  = 0.0f;
    inc = 2.0f * (float)M_PI * 1000.0f / SAMPLE_RATE;
    for (int i = 0; i < DOWN_SAMPLES; i++) {
        float fade = (i < DOWN_SAMPLES - 48) ? 1.0f : (float)(DOWN_SAMPLES - i) / 48.0f;
        int16_t s  = (int16_t)(AMPLITUDE * sinf(ph) * vol * fade);
        downBuf[i * 2]     = s;
        downBuf[i * 2 + 1] = s;
        ph += inc;
    }

    ph  = 0.0f;
    inc = 2.0f * (float)M_PI * 600.0f / SAMPLE_RATE;
    for (int i = 0; i < BEAT_SAMPLES; i++) {
        float fade = (i < BEAT_SAMPLES - 32) ? 1.0f : (float)(BEAT_SAMPLES - i) / 32.0f;
        int16_t s  = (int16_t)(AMPLITUDE * sinf(ph) * vol * fade);
        beatBuf[i * 2]     = s;
        beatBuf[i * 2 + 1] = s;
        ph += inc;
    }
}

// Escreve PCM no DMA (retorna imediatamente; I2S cloca os dados em background)
void playAudio(bool isDownbeat) {
    const int16_t* buf = isDownbeat ? downBuf : beatBuf;
    const size_t   len = isDownbeat ? sizeof(downBuf) : sizeof(beatBuf);
    size_t written;
    i2s_write(I2S_NUM_0, buf, len, &written, portMAX_DELAY);
}

void handleBeat() {
    displayBeat = beatIndex;
    playAudio(beatIndex == 0);
    beatIndex = (beatIndex + 1) % TIME_SIGS[sigIdx].num;
    updateDisplay();
}

// ================================================================
// Layout do display (128 × 64 px):
//   y  0-10 : cabeçalho (modo + status)
//   y 12    : separador
//   y 14-40 : valor grande (logisoso22, baseline 40)
//   y 43-53 : indicadores de beat (N blocos = N tempos do compasso)
//   y 56-62 : barra de volume
void updateDisplay() {
    u8g2.clearBuffer();

    // ── Cabeçalho ──────────────────────────────────────────────
    u8g2.setFont(u8g2_font_6x10_tf);
    const char* labels[] = { "BPM", "COMPASSO", "VOLUME" };
    u8g2.drawStr(0, 10, labels[encMode]);
    u8g2.drawStr(110, 10, appState == RUNNING ? ">" : "||");
    u8g2.drawHLine(0, 12, 128);

    // ── Valor principal (grande, centralizado) ──────────────────
    u8g2.setFont(u8g2_font_logisoso22_tf);
    char vbuf[8];
    switch (encMode) {
        case MODE_BPM:
            snprintf(vbuf, sizeof(vbuf), "%d", bpm);
            break;
        case MODE_COMPASS:
            snprintf(vbuf, sizeof(vbuf), "%d/%d",
                     TIME_SIGS[sigIdx].num, TIME_SIGS[sigIdx].den);
            break;
        case MODE_VOLUME:
            snprintf(vbuf, sizeof(vbuf), "%d", volume);
            break;
    }
    int tw = u8g2.getStrWidth(vbuf);
    u8g2.drawStr((128 - tw) / 2, 40, vbuf);

    // ── Indicadores de beat ─────────────────────────────────────
    // N blocos = N tempos do compasso; beat atual = preenchido.
    {
        const int blockY = 43, blockH = 11;
        uint8_t   n      = TIME_SIGS[sigIdx].num;
        int       gap    = 2;
        int       blockW = (128 - (n - 1) * gap) / n;

        for (uint8_t i = 0; i < n; i++) {
            int bx = i * (blockW + gap);
            if (appState == RUNNING && i == displayBeat) {
                u8g2.drawBox(bx, blockY, blockW, blockH);
            } else {
                u8g2.drawFrame(bx, blockY, blockW, blockH);
            }
        }
    }

    // ── Barra de volume ─────────────────────────────────────────
    {
        const int barY = 56, barH = 7;
        u8g2.drawFrame(0, barY, 128, barH);
        int fill = (volume * 126) / 100;
        if (fill > 0) u8g2.drawBox(1, barY + 1, fill, barH - 2);
    }

    u8g2.sendBuffer();
}

// ================================================================
void onEncoderChange(int delta) {
    delta = -delta;

    switch (encMode) {
        case MODE_BPM:
            bpm = constrain(bpm + delta, BPM_MIN, BPM_MAX);
            if (appState == RUNNING) rearmTimer();
            break;
        case MODE_COMPASS: {
            int idx = (int)sigIdx + delta;
            if (idx < 0)              idx = NUM_SIGS - 1;
            if (idx >= (int)NUM_SIGS) idx = 0;
            sigIdx    = (uint8_t)idx;
            beatIndex = 0;
            break;
        }
        case MODE_VOLUME:
            volume = (uint8_t)constrain((int)volume + delta, 0, 100);
            generateAudio();
            break;
    }
    updateDisplay();
}

void onShortPress() {
    if (appState == IDLE) {
        appState  = RUNNING;
        beatIndex = 0;
        digitalWrite(PIN_MAX_SD, HIGH);  // habilita o amplificador
        rearmTimer();
        handleBeat();
    } else {
        appState = IDLE;
        timerAlarmDisable(beatTimer);
        digitalWrite(PIN_MAX_SD, LOW);   // desliga o amplificador — silêncio total
        displayBeat = 0;
    }
    updateDisplay();
}

void onLongPress() {
    encMode = (EncMode)((encMode + 1) % 3);
    updateDisplay();
}
