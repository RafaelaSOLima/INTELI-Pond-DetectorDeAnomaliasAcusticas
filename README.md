# Detector Acústico de Palavras (ball · cat · dog) com ESP32 + FreeRTOS

Ponderada **Detector de Anomalias Acústicas**. Um sistema embarcado reconhece em
tempo real, **no próprio ESP32 (edge)**, as palavras em inglês *ball*, *cat* e
*dog* faladas por crianças. Qualquer outra palavra ou ruído é tratado como
`unknown`. O computador só exibe a interface do jogo.

> 🚧 Em construção. Etapa atual: **1: hardware e coleta do dataset**.

## Estrutura

```
firmware/
  libraries/kws_common/   código compartilhado: pinagem, constantes de áudio, leitura I2S
  recorder/               ferramenta: teste de hardware + gravação do dataset pelo INMP441
tools/                    scripts de coleta e verificação do dataset
tests/                    testes sem hardware (simulador serial)
docs/                     hardware, guia de coleta, (depois) diagrama RTOS e relatório
dataset/raw/              áudios (fora do git) + manifest.csv
```

## Começando

1. Montagem e testes do hardware: [`docs/hardware.md`](docs/hardware.md)
2. Coleta do dataset: [`docs/coleta_dataset.md`](docs/coleta_dataset.md)
