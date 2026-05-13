# Togepi — Metrônomo Digital com ESP32

Metrônomo digital embarcado com display 7 segmentos, encoder rotativo, 6 LEDs e buzzer piezo passivo. Alimentado por 4 pilhas AA.

---

## Hardware

| Componente | Modelo / Detalhe |
|---|---|
| Microcontrolador | ESP32 WROOM 38 pinos |
| Display | TM1637 4 dígitos, 7 segmentos (montado invertido 180°) |
| Encoder | KY-040 com botão integrado |
| LEDs | 6 × LED com resistor de 150 Ω |
| Buzzer | Piezo passivo |
| Alimentação | 4 × pilha AA (~6 V via pino VIN) |

---

## Pinagem

### Lado 1 — LEDs (posições 7–12, consecutivas)

| Pos | GPIO | Função |
|-----|------|--------|
| 7 | 32 | LED 1 — downbeat |
| 8 | 33 | LED 2 |
| 9 | 25 | LED 3 |
| 10 | 26 | LED 4 |
| 11 | 27 | LED 5 |
| 12 | 14 | LED 6 |

### Lado 2 — Display, Encoder, Buzzer

| Pos | GPIO | Função |
|-----|------|--------|
| 6 | 21 | TM1637 CLK |
| 7 | GND | Referência |
| 8 | 19 | TM1637 DIO |
| 9 | 18 | Encoder CLK (A) |
| 10 | 5 | Encoder DT (B) |
| 11 | 17 | Encoder SW (botão) |
| 12 | 16 | Buzzer (PWM) |

> O display está montado de cabeça para baixo. O firmware já compensa via `rotateSegment()` e `showFlipped()`.

---

## Dependências (platformio.ini)

```ini
lib_deps =
    https://github.com/avishorp/TM1637.git
    madhephaestus/ESP32Encoder@^0.10.1
    mathertel/OneButton@^2.5.0
```

Framework: Arduino-ESP32 v3.20017 / IDF v4.4.7 — usa a **API de timer 1.x** (`timerBegin(num, div, dir)` + `timerAlarmWrite`).

---

## Build e Upload

```bash
# Build
~/.platformio/penv/bin/pio run --project-dir .

# Upload via USB (adicionar usuário ao grupo dialout antes):
# sudo usermod -a -G dialout $USER  → fazer logout e login
~/.platformio/penv/bin/pio run -t upload --project-dir .
```

---

## Uso

| Ação no botão | Resultado |
|---|---|
| Press curto (parado) | Inicia metrônomo |
| Press curto (rodando) | Para metrônomo |
| Press longo | Modo seleção de compasso |
| Qualquer press (modo compasso) | Confirma e sai |

| Encoder | Modo normal | Modo compasso |
|---|---|---|
| Girar | Ajusta BPM (40–300) | Muda compasso |

**Compassos disponíveis:** 2/4 · 3/4 · 4/4 · 5/4 · 6/8

**Display:** BPM com 4 dígitos (`0120`) ou fração (`4 - 4`).

**LEDs:** sequenciais por beat; LED 1 = downbeat (tom grave no buzzer).

---

## Otimizações de Baixo Consumo

- CPU reduzida de 240 MHz para **80 MHz** (`setCpuFrequencyMhz(80)`)
- UART desligada (`Serial.end()`)
- Brilho do display: **1/7**
- WiFi/BT nunca inicializados
- Consumo estimado: **~47 mA** → ≈ 53 h com 4 × AA (2500 mAh)

---

## Problemas Conhecidos

### 1. Direção do encoder invertida

**Sintoma:** Girar no sentido anti-horário aumenta o valor (BPM ou compasso); horário diminui.

**Causa:** A lógica de contagem do `ESP32Encoder` resulta em delta positivo para o sentido físico oposto ao esperado neste hardware específico.

**Correção:** Em `onEncoderChange()` em [src/main.cpp](src/main.cpp), negar o delta em **ambos** os ramos:

```cpp
void onEncoderChange(int delta) {
    delta = -delta;   // ← adicionar esta linha
    if (appState == TIME_SIG) {
        ...
```

---

### 2. Encoder KY-040 conta duas posições por detent

**Sintoma:** A cada "clique" físico do encoder (uma posição de detent), o valor muda duas vezes em vez de uma.

**Causa:** O KY-040 gera 2 pulsos por detent. O método `attachHalfQuad()` acumula 2 contagens por pulso de fase, resultando em 4 incrementos por detent. O método correto para KY-040 é `attachSingleEdge()`, que acumula apenas 1 contagem por pulso, resultando em 2 incrementos — e dividindo o delta por 2 obtemos 1 por detent.

**Correção:** Em `setup()` em [src/main.cpp](src/main.cpp), substituir:

```cpp
// antes:
encoder.attachHalfQuad(PIN_ENC_A, PIN_ENC_B);

// depois:
encoder.attachSingleEdge(PIN_ENC_A, PIN_ENC_B);
```

> **Nota:** Após esta correção, se o encoder ainda contar em 2× ou 0,5× por detent, alternar entre `attachHalfQuad`, `attachSingleEdge` e `attachFullQuad` até encontrar o comportamento correto. O KY-040 tipicamente funciona com `attachSingleEdge`.

---

## Estrutura de Arquivos

```
Togepi/
├── src/
│   └── main.cpp        # Firmware completo
├── platformio.ini      # Configuração do PlatformIO
├── PINAGEM.txt         # Diagrama físico de conexões
└── README.md           # Este arquivo
```
