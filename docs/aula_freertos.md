# Aula: o FreeRTOS deste projeto

Cada conceito vem em duas partes: **o que é** e **neste projeto**. A segunda é
o que você fala na apresentação.

---

## 1. RTOS (Real-Time Operating System)

**O que é.** Um sistema operacional feito para cumprir **prazos**. "Tempo real"
não quer dizer "rápido". Quer dizer **previsível**: a tarefa mais importante
sempre roda quando precisa, dentro de um tempo máximo conhecido.

**Neste projeto.** O prazo crítico é o do microfone: a cada 16 ms chega um bloco
de áudio novo, e o DMA segura no máximo 128 ms. Se o sistema atrasar mais que
isso, o áudio se perde de forma irreversível. O RTOS garante que a captura seja
atendida primeiro, mesmo com a CNN rodando.

## 2. FreeRTOS

**O que é.** Um RTOS pequeno e de código aberto, com escalonador, tasks, filas,
semáforos, mutexes, event groups e stream buffers. No ESP32 ele **já vem dentro**
do ESP-IDF e do core Arduino: o próprio `setup()`/`loop()` do Arduino roda
numa task do FreeRTOS.

**Neste projeto.** Usamos a API do FreeRTOS diretamente (`xTaskCreatePinnedToCore`,
`xQueueSend`, `xSemaphoreTake`…). O `loop()` do Arduino se apaga
(`vTaskDelete(NULL)`), porque todo o trabalho é das nossas 4 tasks.

## 3. Task

**O que é.** Uma função que roda "para sempre" (`for (;;)`), com **pilha
própria** e **prioridade** própria. Para o programador, parece que cada task tem
a CPU inteira para si. Na prática, o escalonador alterna entre elas.

**Neste projeto.** Há 4 tasks, cada uma com **uma responsabilidade**:

| Task | Arquivo | Faz |
|---|---|---|
| T1 captura (prio 5) | `task_capture.cpp` | lê o I2S e escreve no buffer circular |
| T2 features (prio 4) | `task_features.cpp` | RMS, centroid, MFCC, detecção de fala, monta a janela |
| T3 detecção (prio 3) | `task_detect.cpp` | CNN, threshold, LED, resultado JSON |
| T4 comunicação (prio 2) | `task_comm.cpp` | comandos do PC e heartbeat |

Elas são criadas em `setup()` (`kws_rtos.ino`) com
`xTaskCreatePinnedToCore(função, nome, pilha, parâmetro, prioridade, &handle, núcleo)`.

**Por que separar em tasks?** Cada etapa tem um ritmo diferente. A captura
acontece a cada 16 ms, as features também, a inferência só quando alguém fala e
a comunicação quando o PC manda algo. Separadas, uma etapa lenta (a CNN, 32 ms)
não atrasa a etapa urgente (a captura).

## 4. Escalonador (scheduler)

**O que é.** A parte do RTOS que decide **qual task roda agora**. Regra do
FreeRTOS: **roda a task PRONTA de maior prioridade**. Se houver empate, as tasks
se revezam a cada tick de 1 ms.

**Estados de uma task:**
- *Running*: está usando a CPU.
- *Ready*: poderia rodar e está esperando a vez.
- *Blocked*: está esperando algo (fila, tempo, DMA) e **não gasta CPU**.
- *Suspended*: pausada manualmente (não usamos).

**Neste projeto.** Na maior parte do tempo, **todas as 4 estão bloqueadas**:
- a T1 no `i2s_read`;
- a T2 no `xQueueReceive(qBlocks)`;
- a T3 no `xQueueReceive(qWindows)`;
- a T4 no `vTaskDelay`.

Quem roda é a *idle task* do sistema. É por isso que o processamento de áudio
usa só ~4,4% da CPU (710 µs a cada 16 ms).

## 5. Prioridade

**O que é.** Um número; maior quer dizer mais urgente. Define quem ganha a CPU
quando várias tasks estão prontas.

**Neste projeto.**
- **T1 = 5 (a mais alta):** o microfone não espera. Atrasar a captura perde
  áudio para sempre.
- **T2 = 4:** precisa acompanhar o ritmo de 1 frame a cada 16 ms. Se atrasar
  muito, a fila entre T1 e T2 enche.
- **T3 = 3:** roda só quando há uma palavra. Um atraso de alguns milissegundos
  é imperceptível para a criança.
- **T4 = 2 (a mais baixa):** comandos e heartbeat podem esperar.

**Todas ficam no núcleo 1.** O ESP32 tem 2 núcleos. Tasks em núcleos diferentes
rodam em paralelo e **não competem**, e aí a prioridade não faria diferença.
Colocando as quatro no mesmo núcleo, a prioridade decide de verdade quem roda.

## 6. Preempção

**O que é.** Quando uma task de prioridade maior fica pronta, o escalonador
**interrompe na hora** a task de prioridade menor que estava rodando, sem
esperar ela terminar.

**Neste projeto.** A CNN da T3 leva ~32 ms, e durante esse tempo chegam 2 blocos
de áudio. Quando o DMA enche um buffer, a interrupção do I2S acorda a T1, que
**toma a CPU da T3 no meio do cálculo**: converte o bloco (134 µs), coloca na
fila e volta a bloquear. Em seguida a T2 (prio 4) processa o frame, e só então
a T3 continua a CNN. Resultado medido: **zero blocos perdidos** (`dma_gaps = 0`,
`ring_drops = 0`).

## 7. Troca de contexto

**O que é.** Para pausar uma task e rodar outra, o RTOS **salva os registradores
da CPU** (o "contexto") da task atual na pilha dela e **restaura** o contexto da
próxima. Leva microssegundos.

**Neste projeto.** Acontece a cada preempção descrita acima, e toda vez que uma
task bloqueia numa fila ou volta dela.

## 8. Stack (pilha)

**O que é.** A memória **própria de cada task**: variáveis locais, endereços de
retorno e o contexto salvo. O tamanho é definido na criação. Se a task usar mais
que isso, ocorre um *stack overflow* e o sistema trava.

**Neste projeto.** As pilhas foram definidas em `app.h` (`STACK_CAPTURE 3072`,
`STACK_FEATURES 6144`…). **Os buffers grandes são `static`** (a janela de
3,7 KB, os buffers da CNN), então ficam na RAM global e **não** na pilha. O
heartbeat mede o *high water mark* (`uxTaskGetStackHighWaterMark`), que é o
menor espaço livre que a pilha já teve. Os valores medidos, em bytes livres,
foram capture 2396, features 5388, detect 2204 e comm 3764. Todas têm folga.

## 9. Heap

**O que é.** A memória "livre" de onde se aloca dinamicamente. No FreeRTOS, as
**filas, mutexes, event groups e as pilhas das tasks** são alocados no heap
quando são criados.

**Neste projeto.** Tudo é alocado **uma vez**, no `setup()`, e nunca é liberado.
Isso evita fragmentação e falhas de memória no meio da execução. Se uma criação
falha (retorna `NULL`), o firmware para com o LED piscando rápido (`fatal()`).
Heap livre medido: **185 644 bytes**.

## 10. Queue (fila)

**O que é.** Um canal **thread-safe** de mensagens de tamanho fixo, em ordem
FIFO. `xQueueSend` **copia** o item para dentro da fila e `xQueueReceive` copia
para fora. Quem recebe pode **bloquear** até chegar algo, sem gastar CPU.

**Anatomia de um `xQueueSend(fila, &item, timeout)`**, pergunta frequente:
- **Quem criou a fila:** o `setup()`, com `xQueueCreate(tamanho, sizeof(item))`.
- **Onde ela fica:** no heap, com espaço para `tamanho` cópias do item.
- **O que é enviado:** uma **cópia** dos bytes do item.
- **Quando bloqueia:** se a fila estiver cheia, espera até `timeout`.
- **Quando retorna:** `pdTRUE` quando copiou; `errQUEUE_FULL` quando o timeout
  estourou.
- **Quem recebe:** quem chama `xQueueReceive`. Se estava bloqueado esperando,
  é acordado.

**Neste projeto.**
- **`qBlocks` (T1 → T2):** leva o índice do bloco no buffer circular, o número
  de sequência e os tempos. A T1 envia com **timeout 0**: se a fila estiver
  cheia, **descarta e conta** (`ring_drops`), porque a captura nunca pode esperar.
- **`qWindows` (T2 → T3):** leva a **janela inteira copiada** (61×15 floats =
  3,7 KB). A T3 recebe uma cópia só dela, então a T2 pode continuar escrevendo
  features novas sem risco de a T3 ler dados pela metade.

**Sem a fila**, a T2 teria que ficar perguntando o tempo todo "tem dado novo?"
(*polling*, que gasta CPU) e precisaria de outro mecanismo para não ler um bloco
enquanto a T1 o escreve.

## 11. Buffer circular

**O que é.** Um vetor usado em "anel": depois da última posição, volta para a
primeira (`slot = (slot + 1) % N`). É memória fixa, sem alocação, ideal para
fluxo contínuo.

**Neste projeto.** `g_ring[19][256]`: 19 blocos de 16 ms, ou seja, 304 ms de
áudio. A T1 escreve e a T2 lê. A fila `qBlocks` diz **qual** bloco está pronto.
Ela tem **19 − 3 = 16** posições. Mesmo com a fila cheia e a T2 processando um
bloco, sobra folga para a T1 **nunca sobrescrever** um bloco que a T2 ainda vai
ler. A T2 copia o bloco para o próprio frame logo que o recebe.

## 12. Semaphore (semáforo)

**O que é.** Um **contador de sinais**. `Give` incrementa e `Take` decrementa, e
bloqueia se estiver em zero. O semáforo **binário** (0/1) é usado para
"acordar" uma task, por exemplo de dentro de uma interrupção.

**Neste projeto.** **Não usamos semáforo binário, de propósito.** O único uso
natural seria a interrupção do botão acordando uma task, e o botão foi removido.
Colocar um semáforo sem necessidade só para cumprir requisito seria artificial.
Os mutexes que usamos **são** semáforos especiais: `xSemaphoreCreateMutex()`.

## 13. Mutex

**O que é.** Um semáforo para **exclusão mútua**: só quem tem a "chave"
(`xSemaphoreTake`) acessa o recurso, e depois devolve (`xSemaphoreGive`).
Diferenças para o semáforo:
- **tem dono:** só quem pegou devolve;
- **tem herança de prioridade:** se uma task de prioridade baixa segura o mutex
  e uma de prioridade alta espera por ele, a baixa é temporariamente "promovida"
  para terminar logo. Isso evita a **inversão de prioridade**.

**Neste projeto.**
- **`mtxSerial`:** a T3 (resultado) e a T4 (heartbeat) escrevem na mesma USB.
  Sem o mutex, os caracteres das duas mensagens poderiam se intercalar e o PC
  receberia JSON quebrado. Com ele, **cada linha sai inteira** (`serial_line()`).
- **`mtxGame`:** a palavra-alvo é um texto (vários bytes). A T4 escreve quando
  chega `TARGET cat` e a T3 lê ao decidir. Sem o mutex, a T3 poderia ler um
  alvo pela metade.
- As seções protegidas são **curtíssimas**: só copiar o texto. E **nenhuma task
  segura dois mutexes ao mesmo tempo**, o que torna o deadlock impossível.

## 14. Event Group

**O que é.** Um conjunto de **bits** (flags) que várias tasks podem ligar,
desligar e consultar de forma segura. Uma task pode até **esperar** até certos
bits ficarem ligados.

**Neste projeto.** O bit `EVT_LISTENING` significa "o sistema aceita uma nova
palavra?".
- A **T2 lê** o bit a cada frame e **desliga** ao enviar uma janela.
- A **T3 liga** de novo depois do LED e do intervalo de pausa (*cooldown*), ou
  logo em seguida se era ruído.
- É um estado que **várias tasks** alteram. O Event Group torna isso atômico e
  seguro, e evita que o sistema reaja ao eco da própria resposta.

## 15. Task Notification

**O que é.** A forma mais leve de uma task (ou interrupção) acordar outra task
**específica**, sem criar um objeto. Cada task já tem uma "caixinha" embutida.

**Neste projeto.** **Não usamos.** Onde seria possível (T1 → T2), precisamos
mandar **dados** (índice, sequência, tempos) e **acumular** vários avisos se a
T2 atrasar. A fila faz isso naturalmente. A notificação seria uma otimização
sem ganho real aqui.

## 16. Stream Buffer (extra)

**O que é.** Uma fila de **bytes** (e não de itens de tamanho fixo), feita para
**1 escritor e 1 leitor**.

**Neste projeto.** No teste de performance, a T4 recebe áudio pela USB e
escreve no `g_sbInject`. A T1 tira 512 bytes por bloco e usa **no lugar** do
microfone. Assim, o teste passa pelo mesmo caminho do uso real (T1 → T2 → T3 →
LED) com áudios conhecidos.

## 17. Delay e bloqueio

**O que é.** `vTaskDelay(ticks)` **bloqueia** a task por um tempo e **libera a
CPU** para as outras. É diferente de um laço de espera ativa (*busy-wait*), que
queima CPU sem fazer nada.

**Neste projeto.**
- **T3:** o padrão do LED usa `vTaskDelay`. Durante 1 s de LED aceso, **só a T3
  fica parada**; T1 e T2 continuam capturando e processando normalmente.
- **T4:** `vTaskDelay(10 ms)` entre checagens da USB, ou seja, 100 vezes por
  segundo sem desperdiçar CPU.
- Bloqueios em fila (`portMAX_DELAY`) e no DMA (`i2s_read`) também são
  bloqueios: a task dorme até o evento acontecer.

## 18. Sincronização

**O que é.** Coordenar **quando** as tasks agem e **quem** acessa o quê.

**Neste projeto, o fluxo completo de uma palavra:**
1. A interrupção do DMA acorda a **T1** → bloco no buffer circular → índice na `qBlocks`.
2. A **T2** acorda com o índice → features → o VAD detecta fala → busca do pico → janela copiada para a `qWindows` e bit `LISTENING` desligado.
3. A **T3** acorda com a janela → CNN → decisão → lê o alvo (`mtxGame`) → LED → JSON (`mtxSerial`) → pausa → bit `LISTENING` ligado.
4. A **T4**, em paralelo, recebe `TARGET` (`mtxGame`) e envia o heartbeat (`mtxSerial`).

## 19. Concorrência: problemas e como o projeto os evita

| Problema | O que é | Como evitamos | Evidência |
|---|---|---|---|
| **Race condition** | resultado depende de quem chega primeiro na memória compartilhada | filas por **cópia**; mutex no alvo; frame copiado do ring logo ao receber | — |
| **Buffer overflow** | produtor mais rápido que consumidor | fila dimensionada (ring − 3); descarte **contado** | `ring_drops = 0` |
| **Perda de amostras** | DMA sobrescrito antes de ser lido | T1 com a maior prioridade; 128 ms de folga | `dma_gaps = 0` |
| **Starvation** | task que nunca ganha a CPU | as tasks de prioridade alta bloqueiam a maior parte do tempo (~4,4% da CPU) | T4 envia heartbeat a cada 1 s |
| **Deadlock** | duas tasks esperando uma pela outra para sempre | nunca se seguram 2 mutexes ao mesmo tempo | — |
| **Inversão de prioridade** | task alta esperando uma baixa que não roda | mutex com herança de prioridade | — |
| **Bloqueio indevido** | captura parada esperando outra etapa | T1 usa timeout **0** ao enviar | — |
| **Acesso concorrente a contadores** | leitura durante a escrita | 1 escritor por contador; 32 bits alinhados = atômico no ESP32 | — |

---

## Frases prontas para a apresentação

- "Usamos FreeRTOS porque o microfone tem prazo: a cada 16 ms chega um bloco, e
  com 128 ms de atraso perdemos áudio. O RTOS garante que a captura sempre seja
  atendida, mesmo com a CNN calculando."
- "A captura tem a prioridade mais alta porque perda de áudio é irreversível. A
  detecção tem a mais baixa porque alguns milissegundos a mais não fazem
  diferença para a criança."
- "As tasks se comunicam por filas que **copiam** os dados. Assim nenhuma task
  lê um dado que outra ainda está escrevendo."
- "Não usamos só variáveis globais porque elas não avisam a outra task que
  chegou dado novo (teria que fazer polling) e não impedem leitura durante a
  escrita."
- "Medimos zero perda de áudio em 34 mil blocos, cerca de 9 minutos."
