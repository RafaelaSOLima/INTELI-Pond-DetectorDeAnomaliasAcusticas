# Coleta do dataset

## Classes

| Classe | Conteúdo | Resultado no jogo |
|---|---|---|
| `ball`, `cat`, `dog` | a palavra-alvo | acerto ou erro |
| `unknown` | outras palavras (*bowl, bat, duck, banana, bola, gato*...) | TRY AGAIN |
| `noise` | ruído, palmas, tosse, silêncio | ignorado |

- `unknown` existe para que o modelo não seja obrigado a escolher entre as três palavras.
- `noise` é separado de `unknown` porque fala e não-fala são acusticamente diferentes.
- Além disso, previsões com confiança abaixo do limiar viram `unknown`.

## Formato

WAV PCM 16 bits, 16 kHz, mono, 1,5 s por clipe, gravado pelo próprio INMP441
(o mesmo microfone do uso real).

```
dataset/raw/
├── ball/ cat/ dog/ unknown/ noise/
└── manifest.csv      pessoa, classe, palavra, ambiente, distância, níveis
```

Nome do arquivo: `<pessoa>_<palavra>_<dispositivo>_<nº>.wav`. As pessoas são
anônimas (`p1`, `p2`, `p3`, `bg` para ambiente).

## Gravação

```bash
# com o firmware firmware/recorder gravado no ESP32
python3 tools/record_session.py --speaker p1 --port /dev/ttyUSB0 --env sala
python3 tools/record_session.py --speaker bg --noise-only 12 --port /dev/ttyUSB0 --env cozinha
python3 tools/check_dataset.py
```

O LED acende durante a gravação. O script sorteia a distância (perto, médio,
longe) e avisa quando a fala está ausente, cortada ou saturada.

Áudios de celular podem ser convertidos com `tools/import_audio.py` (requer ffmpeg).

## Divisão por pessoa

Cada pessoa fica inteira no treino ou no teste (leave-one-speaker-out). Se a
mesma voz aparecesse nos dois, o modelo poderia acertar pela voz e não pela
palavra, inflando a acurácia.

## Privacidade

Gravações de voz são dados pessoais (LGPD). Os áudios não são versionados
(`.gitignore`); apenas o `manifest.csv` anônimo vai para o repositório.
