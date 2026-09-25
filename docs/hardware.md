# Hardware: montagem e testes

## Componentes

| Componente | Função |
|---|---|
| Placa com módulo **ESP32-WROOM-32U** | Processamento embarcado (edge) |
| **INMP441** | Microfone MEMS digital com saída I2S |
| LED verde + resistor | Resultado correto |
| LED vermelho + resistor | Resultado incorreto / `unknown` |
| Cabo USB (de **dados**) | Alimentação, gravação do firmware e Serial com o computador |

> **Sobre os números de GPIO.** A numeração é do **chip** ESP32, então vale para
> qualquer placa com WROOM-32/32U. Na serigrafia da placa, o mesmo pino pode
> aparecer como `25`, `D25`, `IO25`, `G25` ou `P25`. Se algum GPIO desta tabela
> não aparecer na sua placa, **pare e tire uma foto antes de ligar**.

## Pinos que evitamos (e por quê)

| GPIO | Motivo |
|---|---|
| 6–11 | Ligados à memória flash interna. Usá-los trava a placa. |
| 0, 2, 5, 12, 15 | *Strapping pins*: o nível deles no boot define o modo de inicialização. O GPIO 12 em nível alto, por exemplo, pode impedir o boot. |
| 34–39 | Só entrada (não servem para acionar LED). |
| 1, 3 | TX/RX da Serial USB. |

## Tabela de ligações

### INMP441 (confira os nomes impressos no seu módulo)

| Pino do INMP441 | Liga em | Função |
|---|---|---|
| **VDD** | **3V3** do ESP32 | Alimentação. **NUNCA use 5V/VIN**: o INMP441 aceita até 3,6 V. |
| **GND** | **GND** | Referência comum |
| **SCK** | **GPIO 26** | *Bit clock*: o ESP32 gera 1 pulso por bit (16 kHz × 64 bits = 1,024 MHz) |
| **WS** | **GPIO 25** | *Word select*: indica se o slot atual é o canal esquerdo ou o direito (troca a 16 kHz) |
| **SD** | **GPIO 33** | *Serial data*: os bits do áudio saem do microfone para o ESP32 |
| **L/R** | **GND** | Escolhe o canal: GND = microfone responde no slot **esquerdo** |

### LEDs

```
GPIO 18 ──[ resistor ]──▶|── GND        (LED verde)
GPIO 19 ──[ resistor ]──▶|── GND        (LED vermelho)
                  anodo ▶| catodo
```

- **Polaridade:** a perna **longa** é o **anodo (+)** e vai no lado do resistor/GPIO. A perna **curta**, do lado com o chanfro/face reta no corpo do LED, é o **catodo (−)** e vai no GND. Ao contrário, o LED não acende, mas não queima.
- **Por que o resistor:** o LED quase não limita a própria corrente. Sem resistor, a corrente sobe até estragar o LED ou o pino. Com 3,3 V e um LED vermelho (~2,0 V), um resistor de 220 Ω dá (3,3 − 2,0)/220 ≈ 6 mA. Isso é seguro: o pino do ESP32 tolera ~20 mA com folga.
- **Controle:** `digitalWrite(18, HIGH)` põe 3,3 V no pino, a corrente circula e o LED acende. `LOW` o apaga.

## Cuidados elétricos

1. Monte **com o USB desconectado**. Confira tudo antes de ligar.
2. INMP441 **só em 3V3**.
3. **GND comum**: microfone e LEDs vão ao mesmo GND do ESP32.
4. Fios de I2S **curtos** (idealmente < 15 cm). O SCK a 1 MHz em jumpers longos gera ruído e dados corrompidos.
5. Nenhum LED sem resistor.
6. Use um cabo USB **de dados**. Muitos cabos só carregam, e aí a placa não aparece no computador.

## Preparando o computador (Linux)

```bash
# 1) arduino-cli (compila e grava o firmware)
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.17

# 2) Python (ferramentas de coleta)
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

# 3) Permissão da porta serial (depois faça logout/login)
sudo usermod -aG dialout $USER
# Ubuntu: se a placa aparecer e sumir em seguida, o brltty está "roubando" a porta:
sudo apt remove brltty
```

Para descobrir a porta, rode `ls /dev/ttyUSB* /dev/ttyACM*` antes e depois de plugar a placa.

## Testes isolados, na ordem

Grave o firmware de teste e coleta:

```bash
cd firmware
arduino-cli compile --fqbn esp32:esp32:esp32 --libraries libraries recorder
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 recorder
```

Se o upload travar em `Connecting....`, segure o botão **BOOT** da placa até começar a gravar.

Depois abra o console (`python3 tools/serial_console.py --port /dev/ttyUSB0`) e aperte o botão **EN/RST** da placa.

| # | Teste | Resultado esperado | Se falhar |
|---|---|---|---|
| 1 | **LEDs**: no boot, ou pelo comando `LEDS` | Verde e vermelho piscam alternados 2× | Polaridade invertida? Resistor na fileira certa da protoboard? |
| 2 | **Microfone**: `LEVEL` | Barras `####` crescem ao falar. Silêncio ~ −60 a −50 dBFS, fala perto ~ −30 a −15 dBFS. | Mensagem `audio=FALHOU` no boot, ou nível parado em −120: confira VDD, GND, SCK, WS, SD |
| 3 | **Canal**: `CHAN` | `modo configurado: ... OK, ha sinal` | `SEM SINAL`: confira o L/R no GND. Se o sinal aparecer só no outro slot, troque `AUDIO_CHANNEL_FMT` em `kws_config.h` |
| 4 | **Gravação**: `python3 tools/record_session.py --speaker teste --reps 1 --unknown 1 --noise 1` | Três WAVs em `dataset/raw/` que soam bem com `aplay` | Voz saturada: aumente `AUDIO_SHIFT_24_TO_16` para 6. Voz baixa demais: diminua para 4. **Decida isso ANTES de gravar o dataset.** |

Cole a saída dos testes 2 e 3 na conversa para validarmos juntos.

## Por que não usamos o botão

A rodada é iniciada pela interface no computador, que envia a palavra-alvo ao
ESP32. O ESP32 escuta continuamente e dispara a classificação quando o RMS indica
início de fala. Assim a criança só precisa falar, sem apertar nada. Disparos por
barulho são filtrados pela classe `noise` e pelo limiar de confiança.
