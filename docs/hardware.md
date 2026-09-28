# Hardware

## Componentes

| Componente | Função |
|---|---|
| ESP32-WROOM-32U | Processamento embarcado |
| INMP441 | Microfone MEMS digital (I2S) |
| 1 LED + resistor 220 Ω | Alerta: 1 piscada longa = acerto, 3 curtas = erro |
| Cabo USB de dados | Alimentação, gravação e comunicação serial |

## Ligações

| INMP441 | ESP32 | Função |
|---|---|---|
| VDD | 3V3 | Alimentação (máx. 3,6 V, nunca 5 V) |
| GND | GND | Referência |
| SCK | GPIO 26 | Bit clock (16 kHz × 64 bits = 1,024 MHz) |
| WS | GPIO 25 | Word select (canal esquerdo/direito) |
| SD | GPIO 33 | Dados de áudio |
| L/R | GND | Microfone no slot esquerdo |

```
GPIO 19 ──[ 220 Ω ]──▶|── GND      (perna longa do LED no lado do resistor)
```

Com 3,3 V e LED vermelho (~2,0 V): (3,3 − 2,0) / 220 ≈ 6 mA.

Pinos evitados: 6–11 (flash), 0/2/5/12/15 (strapping), 34–39 (só entrada) e 1/3 (Serial USB).

## Ambiente (Linux)

```bash
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.17

python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

sudo usermod -aG dialout $USER   # permissão da porta serial (refazer login)
```

## Testes de bancada

Firmware de teste (`firmware/recorder`):

```bash
cd firmware
arduino-cli compile --fqbn esp32:esp32:esp32 --libraries libraries recorder
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 recorder
python3 ../tools/serial_console.py --port /dev/ttyUSB0
```

| Comando | Resultado esperado |
|---|---|
| `LEDS` | 1 piscada longa e 3 curtas |
| `LEVEL` | Silêncio ≈ −60 a −50 dBFS; fala próxima ≈ −30 a −15 dBFS |
| `CHAN` | `OK, ha sinal` no slot configurado |
| `SCAN` / `WIRE` / `VOLT` | Diagnóstico de canal, fiação e alimentação |

## Leitura em estéreo

Com o driver I2S legado (core 2.0.x, amostras de 32 bits), os modos mono
`ONLY_LEFT`/`ONLY_RIGHT` não entregaram o áudio com L/R no GND. O `SCAN` mostrou
o sinal no slot 0 da leitura estéreo. Por isso o I2S lê os dois slots e o código
usa `AUDIO_MIC_SLOT = 0` (`kws_config.h`). O ganho é ajustado por
`AUDIO_SHIFT_24_TO_16 = 8`.
