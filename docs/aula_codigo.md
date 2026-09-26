# Aula: o código, arquivo por arquivo

Ordem sugerida de leitura: **config → áudio → features → modelo → tasks →
treino → testes → interface**.

---

## Parte 1: biblioteca compartilhada `firmware/libraries/kws_common/src/`

Código usado tanto pelo **gravador do dataset** quanto pelo **firmware final**.
É isso que garante que o áudio do treino é igual ao do uso real.

### `kws_config.h`
- **Finalidade:** todas as constantes de hardware e de áudio num lugar só.
- **Principais:**
  - `PIN_I2S_SCK/WS/SD` = 26/25/33 e `PIN_LED` = 19;
  - `AUDIO_SAMPLE_RATE` = 16000 e `AUDIO_BLOCK_SAMPLES` = 256 (16 ms);
  - `I2S_DMA_BUF_COUNT` × `I2S_DMA_BUF_LEN` = 8 × 256 (128 ms de folga);
  - `AUDIO_SHIFT_24_TO_16` = 8 (ganho 1×, calibrado com `LEVEL`);
  - `AUDIO_MIC_SLOT` = 0 (medido com `SCAN`);
  - `LED_OK_MS` e `LED_FAIL_*` (os padrões do LED).
- **Relação com o modelo:** mudar o ganho ou a taxa de amostragem depois de
  gravar o dataset exige retreinar. Por isso há o aviso "NÃO mude".
- **Pergunta provável:** *"Por que 16 kHz?"* A informação que distingue as
  palavras está abaixo de 8 kHz, que é o limite de Nyquist de 16 kHz. Uma taxa
  maior só gastaria memória e CPU.

### `audio_io.h / audio_io.cpp`
- **Finalidade:** configurar o periférico I2S e converter as amostras.
- **Funções:**
  - `audio_begin()`: instala o driver I2S em modo **mestre e recepção**, estéreo,
    32 bits, com DMA de 8×256, e define os pinos.
  - `audio_read_raw(raw, n, timeout)`: chama `i2s_read`, que **bloqueia a task**
    até o DMA ter os dados. Quem acorda a task é a interrupção do I2S.
  - `audio_convert(raw, out, n)`: pega o slot do microfone, faz `>> 8` (24 bits
    úteis), aplica o **DC blocker** `y = x − x₋₁ + 0,995·y₋₁`, divide pelo ganho
    e **satura** em int16.
  - `audio_flush()`: descarta o áudio antigo acumulado no DMA (usado pelo gravador).
- **Variáveis:** `s_dc_x1` e `s_dc_y1` guardam o estado do filtro entre blocos.
  Só a T1 chama esta função, então não há concorrência.
- **Pergunta provável:** *"Por que ler em estéreo se o microfone é mono?"* Os
  modos mono do driver antigo não entregavam o canal certo; o diagnóstico
  `SCAN` provou isso. Lendo os dois slots e escolhendo o 0, não dependemos da
  convenção do driver.

### `kws_features.h / kws_features.c`
- **Finalidade:** calcular as 15 features de um frame. É **C puro**, sem
  Arduino, então compila no ESP32 e no PC (para o teste de equivalência).
- **Constantes:** frame 512, passo 256, 40 filtros mel, 13 MFCC, janela de
  61 frames, parâmetros do VAD (+10 dB, −60 dBFS, 2 frames) e do alinhamento
  (busca de 45 frames, pico no frame 15).
- **Funções:**
  - `kws_features_init()`: pré-calcula a janela de Hann, os *twiddles* da FFT,
    a inversão de bits, o banco mel **esparso** (só os pesos não nulos) e a
    matriz DCT. É chamada **uma vez**, antes das tasks.
  - `fft()`: FFT radix-2 iterativa de 512 pontos.
  - `kws_features_frame(frame, feat)`, em 5 passos:
    1. RMS;
    2. janela de Hann;
    3. FFT e potência;
    4. centroid;
    5. mel → log → DCT = MFCC.
    Saída: `[mfcc0..12, log10(RMS), centroid/8000]`.
  - `kws_vad_update(v, log_rms)`: detector de fala com piso adaptativo. Retorna
    1 só no frame em que a fala **começa**.
- **Relação com o FreeRTOS:** usa buffers `static`, portanto **não é reentrante**.
  Por projeto, só a T2 chama.
- **Pergunta provável:** *"Como você garante que o ESP32 calcula igual ao treino?"*
  Com `tests/test_features.py`: compila este C com gcc e compara com o Python.
  Erro máximo de 4,8·10⁻⁴.

### `kws_model.h / kws_model.c` e `kws_model_weights.h`
- **Finalidade:** executar a CNN em C. Os pesos são **gerados a partir do
  `kws.onnx`** por `training/export_c.py`.
- **Funções:**
  - `conv3x3_relu`: convolução 3×3 "same" (padding com zeros) + ReLU.
  - `maxpool2`: pooling 2×2 que descarta a sobra ímpar, igual ao ONNX.
  - `dense`: camada totalmente conectada (`Gemm` com `transB`).
  - `kws_model_run(in, probs)`: normalização → conv → pool → conv → pool →
    dense → dense → **softmax estável** (subtrai o máximo antes do `exp`).
- **Memória:** os pesos são `static const` e ficam na **flash**. Os buffers
  intermediários são `static` e ficam na RAM global, fora da pilha.
- **Pergunta provável:** *"Por que não rodar o ONNX direto?"* Não há runtime
  ONNX maduro para ESP32. O `.onnx` é a fonte dos pesos, e o teste prova que o
  C dá o mesmo resultado que o `onnxruntime` (diferença de 2,4·10⁻⁶).

---

## Parte 2: firmware principal `firmware/kws_rtos/`

### `app.h`
- **Finalidade:** o "contrato" entre as tasks: prioridades, tamanhos de pilha,
  tipos de mensagem e handles globais.
- **Estruturas:**
  - `BlockMsg {slot, seq, t_ready_us, read_us}`: item da fila T1→T2. Leva o
    **índice** do bloco, não o áudio.
  - `WindowMsg {feats[61*15], seq, t_onset_us, …}`: item da fila T2→T3. Leva a
    **janela inteira** mais os tempos para calcular a latência.
  - `Stats`: contadores, **um escritor por campo**.
- **Pergunta provável:** *"Por que a fila de blocos tem tamanho RING − 3?"* Para
  que, mesmo com a fila cheia e a T2 processando um bloco, a T1 nunca escreva
  num bloco que ainda vai ser lido.

### `kws_rtos.ino`
- **Finalidade:** a inicialização.
- **Fluxo do `setup()`:**
  1. Serial a 921600, com buffer de recepção de 16 KB.
  2. `kws_features_init()` e `audio_begin()`.
  3. Cria as 2 filas, os 2 mutexes, o event group e o stream buffer, **antes**
     das tasks.
  4. Liga o bit `EVT_LISTENING`.
  5. Envia o JSON de boot.
  6. Cria as 4 tasks **fixadas no núcleo 1**, consumidoras primeiro.
- **`loop()`:** `vTaskDelete(NULL)`, porque a task do Arduino não é necessária.
- **`serial_line()`:** escreve uma linha inteira com o `mtxSerial`.
- **`fatal()`:** se uma criação falhar, o LED pisca rápido para sempre.
- **Pergunta provável:** *"Por que criar as filas antes das tasks?"* Para
  nenhuma task usar um handle ainda nulo.

### `task_capture.cpp` (T1, prioridade 5)
- **Laço:**
  1. `audio_read_raw` (**bloqueia** até o DMA).
  2. Confere a folga do DMA (`dma_gaps`).
  3. `audio_convert` direto para `g_ring[slot]`.
  4. Se houver áudio injetado pelo teste, ele substitui o bloco.
  5. `xQueueSend(qBlocks, …, 0)`: **timeout 0**; se falhar, conta `ring_drops`.
  6. `slot = (slot + 1) % 19`.
- **Latência medida:** `read_us` = conversão (134 µs).
- **Pergunta provável:** *"O que acontece se a captura for mais rápida que o
  processamento?"* Os blocos se acumulam no buffer circular (até ~300 ms). Se
  ainda assim a fila encher, a T1 **descarta e conta**, mas nunca trava a
  captura. Medido: 0 descartes.

### `task_features.cpp` (T2, prioridade 4)
- **Laço:**
  1. `xQueueReceive(qBlocks)` (**bloqueia**).
  2. Detecta perda pela sequência (`seq_gaps`).
  3. Monta o frame = bloco anterior + bloco atual, **copiando** do ring.
  4. `kws_features_frame` → histórico circular `s_hist[128]`.
  5. `kws_vad_update`.
  6. Se houve **início de fala** e o bit `LISTENING` está ligado, começa a
     **busca do pico** por 45 frames.
  7. Quando a busca termina e os 61 frames existem, copia a janela, **desliga**
     o `LISTENING` e faz `xQueueSend(qWindows)`.
- **Pergunta provável:** *"Por que alinhar pelo pico e não pelo início da
  fala?"* O detector às vezes dispara em ruído antes da palavra. O pico de
  energia é o núcleo da palavra e cai no mesmo lugar que no treino. Essa foi a
  correção que levou a simulação a 89%.

### `task_detect.cpp` (T3, prioridade 3)
- **Laço:**
  1. `xQueueReceive(qWindows)` (**bloqueia**).
  2. `kws_model_run` (32 ms, medido).
  3. Classe vencedora:
     - `noise` → envia `ignored`, **sem LED**, e liga o `LISTENING`;
     - ball/cat/dog com confiança ≥ 0,30 → a palavra;
     - qualquer outro caso → `unknown`.
  4. Lê o alvo com o `mtxGame` (só copia).
  5. `ok = (palavra == alvo)`; no modo livre, qualquer palavra conhecida vale.
  6. LED aceso → **tempo final da latência** (`end_to_led`).
  7. JSON com o `mtxSerial`.
  8. Padrão do LED com `vTaskDelay`, depois a pausa, depois liga o `LISTENING`.
- **Pergunta provável:** *"Como o LED sabe que a palavra está correta?"* A T3
  compara a palavra decidida pelo modelo com o alvo que a interface enviou
  (`TARGET cat`), lido com o mutex. A comparação acontece **no ESP32**.

### `task_comm.cpp` (T4, prioridade 2)
- **Laço:** lê a USB sem bloquear, monta linhas e trata os comandos:
  - `TARGET x`: grava o alvo com o `mtxGame`.
  - `PING n`: responde `pong`; o PC mede o tempo de ida e volta.
  - `STATS`: envia os contadores.
  - `INJ n`: recebe `n` bytes de áudio e passa para o stream buffer, "freado"
    pelo ritmo da T1.
- A cada 1 s envia o **heartbeat** (contadores, tempos, heap e sobra de pilha).
  Depois, `vTaskDelay(10 ms)`.
- **Pergunta provável:** *"Como o PC percebe a perda de comunicação?"* Se ficar
  3 s sem heartbeat, a interface mostra "desconectado".

---

## Parte 3: treino `training/`

| Arquivo | Finalidade |
|---|---|
| `features.py` | espelho do C; **lê as constantes do próprio `kws_features.h`** |
| `labels.py` | lista das 5 classes (sem dependências) |
| `dataset.py` | lê o manifest e os WAVs; `pad_clip` (preenche com o trecho mais silencioso); `eval_start` (janela pelo pico); augmentation (ganho, ruído de ambiente, deslocamento) |
| `model.py` | a CNN em PyTorch (a normalização fica dentro do grafo) |
| `train.py` | validação leave-one-speaker-out, ablação, escolha do threshold, modelo final, export `.onnx`, `report.md` |
| `export_c.py` | **confere o grafo ONNX** (13 operações) e gera `kws_model_weights.h` |

- **Pergunta provável:** *"Por que não misturar as gravações da mesma pessoa
  entre treino e teste?"* O modelo aprenderia a **voz** e o **microfone** da
  pessoa, e a acurácia ficaria inflada. Isso foi medido: 82,7% na divisão
  aleatória contra 47,4% por pessoa.

## Parte 4: testes `tests/`

| Arquivo | Prova |
|---|---|
| `test_features.py` | features C = Python |
| `test_model_c.py` | CNN em C = onnxruntime |
| `perf_test.py` | acurácia e latência **no ESP32**, injetando WAVs pela USB (ou no PC com `--offline`) |
| `make_fake_dataset.py`, `fake_recorder.py` | testes sem hardware |

## Parte 5: interface `interface/`

- `server.py`: ponte USB → navegador (Server-Sent Events + POST). **Não
  processa áudio.**
- `index.html`:
  - envia `TARGET`;
  - mostra o resultado (CORRECT / TRY AGAIN) e o placar;
  - detecta ausência de heartbeat;
  - tem um painel técnico com as latências.

## Parte 6: ferramentas `tools/` e o gravador `firmware/recorder/`

- `recorder.ino`: sem FreeRTOS, porque é uma **ferramenta**. Comandos:
  - `LEVEL` (nível do microfone);
  - `CHAN` e `SCAN` (qual slot/pino tem sinal);
  - `WIRE` (procura curtos);
  - `VOLT` (mede tensão pelo ADC);
  - `REC` (grava o clipe).
- `record_session.py`: sessão guiada de gravação, com checagem de qualidade.
- `check_dataset.py`: confere o formato e as contagens do dataset.
- `fix_manifest_speakers.py`: corrige e anonimiza as pessoas no manifest.
