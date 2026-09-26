# Resultados do treino (2026-09-26T12:39:12)

- Clipes: 535 | pessoas: 3 | por classe: {'ball': 100, 'cat': 100, 'dog': 100, 'unknown': 150, 'noise': 85}
- Modelo: 24485 parâmetros, 331000 MACs por inferência
- Avaliação: leave-one-speaker-out (cada pessoa testada com modelo que nunca ouviu a voz dela)

## Features: all (15 por frame)

- Acurácia 5 classes: **0.474**
- Threshold escolhido: **0.3**
- Decisão final (ball/cat/dog/unknown): acurácia **0.514**, macro-F1 0.464, falso aceite 0.205, recall das palavras 0.327

Validação agrupada por: **pessoa real**

| grupo de teste | clipes de teste | acurácia 5 classes |
|---|---|---|
| p1 | 300 | 0.400 |
| p2 | 100 | 0.640 |
| p3 | 100 | 0.530 |

Matriz de confusão (5 classes, argmax):

| real \ previsto | ball | cat | dog | unknown | noise |
|---|---|---|---|---|---|
| **ball** | 36 | 0 | 3 | 52 | 9 |
| **cat** | 0 | 40 | 1 | 50 | 9 |
| **dog** | 16 | 0 | 22 | 45 | 17 |
| **unknown** | 12 | 13 | 13 | 94 | 18 |
| **noise** | 0 | 2 | 1 | 2 | 45 |

Matriz de confusão da decisão final (com threshold):

| real \ previsto | ball | cat | dog | unknown |
|---|---|---|---|---|
| **ball** | 36 | 0 | 3 | 61 |
| **cat** | 0 | 40 | 1 | 59 |
| **dog** | 16 | 0 | 22 | 62 |
| **unknown** | 12 | 15 | 14 | 159 |

## Features: mfcc (13 por frame)

- Acurácia 5 classes: **0.434**
- Threshold escolhido: **0.35**
- Decisão final (ball/cat/dog/unknown): acurácia **0.502**, macro-F1 0.425, falso aceite 0.155, recall das palavras 0.273

Validação agrupada por: **pessoa real**

| grupo de teste | clipes de teste | acurácia 5 classes |
|---|---|---|
| p1 | 300 | 0.327 |
| p2 | 100 | 0.630 |
| p3 | 100 | 0.560 |

Matriz de confusão (5 classes, argmax):

| real \ previsto | ball | cat | dog | unknown | noise |
|---|---|---|---|---|---|
| **ball** | 33 | 0 | 0 | 54 | 13 |
| **cat** | 0 | 37 | 1 | 39 | 23 |
| **dog** | 14 | 0 | 12 | 51 | 23 |
| **unknown** | 14 | 12 | 5 | 88 | 31 |
| **noise** | 0 | 1 | 1 | 1 | 47 |

Matriz de confusão da decisão final (com threshold):

| real \ previsto | ball | cat | dog | unknown |
|---|---|---|---|---|
| **ball** | 33 | 0 | 0 | 67 |
| **cat** | 0 | 37 | 1 | 62 |
| **dog** | 14 | 0 | 12 | 74 |
| **unknown** | 14 | 12 | 5 | 169 |

## Features: all_por_sessao (15 por frame)

- Acurácia 5 classes: **0.596**
- Threshold escolhido: **0.45**
- Decisão final (ball/cat/dog/unknown): acurácia **0.626**, macro-F1 0.605, falso aceite 0.245, recall das palavras 0.540

Validação agrupada por: **sessão de gravação (otimista)**

| grupo de teste | clipes de teste | acurácia 5 classes |
|---|---|---|
| p1_fino | 100 | 0.540 |
| p1_grave | 100 | 0.590 |
| p1_normal | 100 | 0.680 |
| p2 | 100 | 0.640 |
| p3 | 100 | 0.530 |

Matriz de confusão (5 classes, argmax):

| real \ previsto | ball | cat | dog | unknown | noise |
|---|---|---|---|---|---|
| **ball** | 49 | 1 | 7 | 38 | 5 |
| **cat** | 0 | 76 | 5 | 16 | 3 |
| **dog** | 17 | 5 | 38 | 30 | 10 |
| **unknown** | 13 | 14 | 22 | 90 | 11 |
| **noise** | 1 | 3 | 0 | 1 | 45 |

Matriz de confusão da decisão final (com threshold):

| real \ previsto | ball | cat | dog | unknown |
|---|---|---|---|---|
| **ball** | 49 | 1 | 7 | 43 |
| **cat** | 0 | 76 | 4 | 20 |
| **dog** | 17 | 5 | 37 | 41 |
| **unknown** | 12 | 16 | 21 | 151 |
