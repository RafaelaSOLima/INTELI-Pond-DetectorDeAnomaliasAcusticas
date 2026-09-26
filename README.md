# Detector Acústico de Palavras (ball · cat · dog) com ESP32 + FreeRTOS

Ponderada **Detector de Anomalias Acústicas**. Um ESP32 com microfone INMP441
reconhece em tempo real, **no próprio chip (edge)**, as palavras em inglês
*ball*, *cat* e *dog* faladas por crianças. Outras palavras viram `unknown`
(TRY AGAIN) e ruídos são ignorados. O computador só exibe o jogo.

📄 **Relatório técnico:** [`docs/relatorio.md`](docs/relatorio.md) ·
🧩 **Diagrama RTOS:** [`docs/rtos_diagram.svg`](docs/rtos_diagram.svg) ·
🧠 **Modelo:** [`model/kws.onnx`](model/kws.onnx)

## Estrutura

```
firmware/
  kws_rtos/               FIRMWARE PRINCIPAL: 4 tasks FreeRTOS (captura, features, detecção, comunicação)
  recorder/               ferramenta: testes de hardware (LEVEL, CHAN, SCAN, WIRE, VOLT) + gravação do dataset
  libraries/kws_common/   código compartilhado: pinagem, I2S, features (C), inferência da CNN (C), pesos
training/                 features.py (espelho do C), dataset, treino, export ONNX -> C
model/                    kws.onnx, metadata.json, report.md (métricas da validação)
interface/index.html      jogo para a criança (Web Serial, Chrome/Edge)
tests/                    perf_test.py (acurácia/latência no ESP32), test_features.py, test_model_c.py
tools/                    gravação e verificação do dataset
docs/                     relatório, diagrama, hardware, coleta, resultados
dataset/raw/              manifest.csv (anônimo); os WAVs ficam fora do git
```

## Como usar

```bash
# 1) compilar e gravar o firmware (arduino-cli + esp32:esp32@2.0.17, ver docs/hardware.md)
cd firmware
arduino-cli compile --fqbn esp32:esp32:esp32 --libraries libraries kws_rtos
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 kws_rtos
cd ..

# 2) jogar: ponte USB -> navegador (funciona em qualquer navegador)
python3 interface/server.py --port /dev/ttyUSB0     # depois abra http://localhost:8000
#    (alternativa: abrir interface/index.html direto no Chrome, via Web Serial)

# 3) medir acurácia e latência no ESP32 (injeta gravações pela USB)
python3 tests/perf_test.py --port /dev/ttyUSB0 --per-class 10

# 4) testes sem hardware
python3 tests/test_features.py      # features C == Python
python3 tests/test_model_c.py       # inferência C == onnxruntime
python3 tests/perf_test.py --offline

# 5) retreinar (precisa de torch/onnx) e regenerar os pesos em C
python3 training/train.py --root dataset/raw --out model --ablation
python3 training/export_c.py
```

## Documentação

- [Hardware: pinagem, cuidados e testes](docs/hardware.md)
- [Coleta do dataset](docs/coleta_dataset.md)
- [Relatório técnico](docs/relatorio.md)
- [Aula: o FreeRTOS deste projeto](docs/aula_freertos.md)
- [Aula: o código, arquivo por arquivo](docs/aula_codigo.md)
