# Detector Acústico de Palavras com ESP32 + FreeRTOS

Ponderada **Detector de Anomalias Acústicas**. Um ESP32 com microfone INMP441
reconhece em tempo real, no próprio chip, as palavras em inglês *ball*, *cat* e
*dog*. Outras palavras são classificadas como `unknown` e ruídos são ignorados.
O computador apenas exibe o jogo.

## Entregáveis

| Item | Arquivo |
|---|---|
| Relatório técnico | [`docs/relatorio.md`](docs/relatorio.md) |
| Diagrama RTOS | [`docs/rtos_diagram.svg`](docs/rtos_diagram.svg) |
| Modelo | [`model/kws.onnx`](model/kws.onnx) |
| Script de teste | [`tests/perf_test.py`](tests/perf_test.py) |
| Resultados medidos | [`docs/results/`](docs/results/) |

## Estrutura

```
firmware/
  kws_rtos/               firmware principal: 4 tasks FreeRTOS
  recorder/               testes de hardware e gravação do dataset
  libraries/kws_common/   pinagem, I2S, features, inferência da CNN e pesos
training/                 extração de features, treino e exportação ONNX -> C
model/                    kws.onnx, metadata.json, report.md
interface/                jogo (index.html) e ponte USB (server.py)
tests/                    teste de desempenho e testes de equivalência C/Python
tools/                    gravação e verificação do dataset
docs/                     relatório, diagrama, hardware, coleta e resultados
dataset/raw/              manifest.csv (os áudios não ficam no repositório)
```

## Como usar

Requisitos: `arduino-cli` com o core `esp32:esp32@2.0.17` e `pip install -r requirements.txt`.

```bash
# 1) compilar e gravar o firmware
cd firmware
arduino-cli compile --fqbn esp32:esp32:esp32 --libraries libraries kws_rtos
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 kws_rtos
cd ..

# 2) abrir o jogo (depois acesse http://localhost:8000)
python3 interface/server.py --port /dev/ttyUSB0

# 3) medir acurácia e latência no ESP32
python3 tests/perf_test.py --port /dev/ttyUSB0 --per-class 10

# 4) testes sem hardware
python3 tests/test_features.py
python3 tests/test_model_c.py
python3 tests/perf_test.py --offline

# 5) retreinar e regenerar os pesos em C
python3 training/train.py --root dataset/raw --out model --ablation
python3 training/export_c.py
```

## Documentação

- [Hardware e pinagem](docs/hardware.md)
- [Coleta do dataset](docs/coleta_dataset.md)
- [Relatório técnico](docs/relatorio.md)
