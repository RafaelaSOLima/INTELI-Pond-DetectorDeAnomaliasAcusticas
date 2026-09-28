# Teste de performance (offline) — 2026-09-26T12:43:21

- Eventos simulados: 505 | acurácia (ball/cat/dog/unknown): **0.893**
- Eventos sem detecção de fala (VAD não disparou, contados como unknown): 2
- Eventos classificados como ruído e ignorados (sem LED, contados como unknown): 93

| real \ previsto | ball | cat | dog | unknown |
|---|---|---|---|---|
| **ball** | 91 | 0 | 0 | 10 |
| **cat** | 0 | 90 | 0 | 11 |
| **dog** | 1 | 0 | 82 | 18 |
| **unknown** | 5 | 7 | 2 | 188 |

## Latência (µs)

| etapa | n | média | mediana | p95 | máx |
|---|---|---|---|---|---|
| inference | 503 | 395 | 383 | 468 | 976 |
