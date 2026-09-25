# Guia de coleta do dataset

## Classes (e por que são 5)

| Classe | O que contém | Na interface |
|---|---|---|
| `ball` | a palavra *ball* | BALL |
| `cat` | a palavra *cat* | CAT |
| `dog` | a palavra *dog* | DOG |
| `unknown` | **outras palavras**: parecidas (*bowl, bat, doll, duck*), comuns (*apple, banana, hello*) e em português (*bola, gato, cachorro*) | TRY AGAIN |
| `noise` | **não-fala**: silêncio, ruído do ambiente, palmas, tosse, respiração, batidas na mesa | TRY AGAIN / ignorado |

**Por que ter `unknown` como classe?** Um classificador só com `ball/cat/dog`
é *obrigado* a escolher uma das três. Ao ouvir "banana", ele responderia a
menos errada, talvez *ball*. Com exemplos de "outras palavras" no treino, ele
aprende uma região de "não é nenhuma delas".

**Por que separar `noise` de `unknown`?** Fala e não-fala são acusticamente
muito diferentes: fala tem harmônicos e formantes, e ruído não. Juntar os dois
numa classe só obriga o modelo a aprender um grupo muito heterogêneo. Separados,
cada classe fica mais "compacta" e o modelo aprende melhor. Na saída, os dois
viram TRY AGAIN.

**Silêncio** fica dentro de `noise`. No sistema final, o silêncio quase nunca chega
ao modelo, porque o RMS (detector de fala) só dispara a classificação quando há
energia. Ainda assim, precisamos de exemplos de ruídos que *passam* pelo
detector de energia (palma, tosse, porta).

Além dessas classes, o firmware aplica um **limiar de confiança**: se a maior
probabilidade for baixa, o resultado vira `unknown`. Isso cobre sons que nunca
apareceram no treino.

## Quantidade

| Item | Recomendado | Por quê |
|---|---|---|
| Pessoas | **10** (o que você tem) | O teste precisa de pessoas que o modelo nunca ouviu |
| Por pessoa: cada palavra-alvo | **20** repetições | Variação dentro da mesma voz |
| Por pessoa: `unknown` | **30** palavras diferentes | Cobrir muitas "não-palavras" |
| Por pessoa: `noise` | **10** clipes | Ruídos humanos |
| Ambiente sem pessoa (`--speaker bg`) | **40** clipes em 3–4 cômodos | Ruído de fundo |
| Duração de cada clipe | **1,5 s** | A palavra dura ~0,3–0,7 s. A folga permite recortar a janela de 1 s em volta dela. |

Total esperado: ~1000 clipes, ~7 min por pessoa.

## Formato

- **WAV PCM 16 bits, 16 000 Hz, mono.**
  - **16 kHz** é o padrão em reconhecimento de fala: a informação que distingue palavras está abaixo de 8 kHz, que é a metade da taxa (limite de Nyquist). Taxas maiores só aumentam memória e processamento no ESP32.
  - **Mono** porque há um único microfone.
  - **16 bits** porque é suficiente para a faixa dinâmica da fala e é o que o firmware usa.
- O `record_session.py` já grava assim. Arquivos de celular são convertidos pelo `import_audio.py`.

## Organização e nomes

```
dataset/raw/
├── ball/     s03_ball_esp32_007.wav
├── cat/      s03_cat_esp32_012.wav
├── dog/
├── unknown/  s03_banana_esp32_001.wav
├── noise/    s03_palma_esp32_002.wav   bg_ambiente_esp32_010.wav
└── manifest.csv   (pessoa, classe, palavra, dispositivo, ambiente, distância, níveis)
```

Nome do arquivo: `<pessoa>_<palavra>_<dispositivo>_<nº da gravação>.wav`.

- **Pessoas são anônimas** (`s01`…`s10`). Guarde a relação nome ↔ ID fora do repositório.
- **Não existem pastas `train/`, `validation/` e `test/`.** A divisão é feita **por pessoa** num arquivo gerado pelo script de treino. Assim é impossível misturar os conjuntos por engano, e dá para refazer a divisão sem copiar arquivos.

## Por que dividir por pessoa (vazamento de dados)

Se a pessoa `s03` tiver gravações no treino **e** no teste, o modelo pode
acertar o teste reconhecendo **a voz e o microfone da s03**, e não a palavra.
A acurácia medida fica inflada e cai com uma criança nova na demonstração.

Por isso cada pessoa fica inteira em **um** conjunto. Por exemplo:

- 6 pessoas em treino, 2 em validação e 2 em teste;
- validação cruzada por pessoa, para uma estimativa mais estável com só 10 vozes.

## Diversidade: por que importa

O modelo só generaliza para variações que viu. Se todas as gravações forem
de adultos falando devagar a 20 cm numa sala silenciosa, uma criança falando
rápido a 60 cm com a TV ligada será classificada como `unknown`.

Varie **de propósito**:

- **Pessoas e vozes:** homens, mulheres, vozes agudas e graves, e crianças, se houver autorização dos responsáveis.
- **Velocidade:** "cat" rápido e "caaat" arrastado.
- **Volume:** normal, baixo e alto (sem gritar colado no microfone).
- **Sotaque:** pronúncia "à brasileira" também. É assim que as crianças vão falar.
- **Distância:** o script sorteia **PERTO (~15 cm), MÉDIO (~40 cm) ou LONGE (~80 cm)** a cada item. Respeite.
- **Ambiente:** grave pessoas em cômodos diferentes (`--env sala`, `--env quarto`...).

## Como gravar (ESP32 + INMP441, preferencial)

Esse método é preferido porque o modelo vai ouvir **por este microfone** no uso real.

1. Faça os testes 1–5 de [`hardware.md`](hardware.md).
2. Para cada pessoa:
   ```bash
   python3 tools/record_session.py --speaker s01 --port /dev/ttyUSB0 --env sala
   ```
3. O **LED verde acende enquanto grava**. A pessoa fala a palavra da tela **depois de o LED acender** e antes de ele apagar (1,5 s).
4. Se aparecer um aviso (`NÃO detectei fala`, `CORTADA`, `SATURADO`), tecle `r` para regravar.
5. Pode parar com `q` e continuar depois com o mesmo `--speaker`. Nada é sobrescrito.
6. Ruído de fundo, sem ninguém falando, em 3–4 cômodos:
   ```bash
   python3 tools/record_session.py --speaker bg --noise-only 12 --port /dev/ttyUSB0 --env cozinha
   ```

**Microfone do notebook (plano B):** use o mesmo comando com `--source mic`.

**Celular / WhatsApp** (pessoas que não podem estar presentes):

1. A pessoa grava **um áudio por palavra**, repetindo a palavra ~20 vezes com ~1 s de pausa: "cat … cat … cat …".
2. Para `unknown`, pode ser um áudio com várias palavras diferentes separadas por pausas.
3. Pode usar o gravador de voz nativo do celular (m4a) ou um áudio de WhatsApp (ogg/opus).
4. Transfira por cabo USB, Google Drive ou WhatsApp Web ("baixar").
5. Converta e corte:
   ```bash
   sudo apt install ffmpeg
   python3 tools/import_audio.py cat_joao.m4a --speaker s11 --word cat
   python3 tools/import_audio.py outras_joao.ogg --speaker s11 --word varias --label unknown
   ```

> Áudio de celular soa diferente do INMP441. Use-o como **complemento**, nunca como a única fonte de uma classe. Se todos os `unknown` vierem do celular e todos os `cat` do ESP32, o modelo aprende a reconhecer *o microfone*, não a palavra.

## Verificação

```bash
python3 tools/check_dataset.py
```

O script confere formato (16 kHz, mono, 16 bits), duração, arquivos sem
metadados e mostra a tabela de clipes por pessoa × classe.

**Ouça por amostragem:**

```bash
aplay dataset/raw/cat/s01_cat_esp32_00*.wav
```

## Privacidade

Gravações de voz são **dados pessoais** (LGPD), e as de crianças exigem
autorização dos responsáveis. Os WAVs **não são versionados no git**
(`.gitignore`). Só o `manifest.csv`, com IDs anônimos, vai para o repositório.
