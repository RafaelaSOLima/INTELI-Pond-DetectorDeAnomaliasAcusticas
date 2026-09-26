#!/usr/bin/env python3
"""Corrige o manifest.csv da coleta real: as sessões foram gravadas todas com
--speaker s01 e o nome da sessão foi parar em --env. Este script:

  1) define a PESSOA real (speaker) a partir do env, de forma anônima (p1, p2, p3);
  2) guarda a sessão em env (ex.: p1_fino), sem nomes próprios;
  3) marca como speaker "teste" os clipes do teste inicial (ignorados no treino).

    python3 tools/fix_manifest_speakers.py dataset/raw/manifest.csv
"""
import csv
import sys

# env original -> (pessoa anônima, sessão anônima)
MAP = {
    "euNormalShare": ("p1", "p1_normal"),
    "euFinoShare": ("p1", "p1_fino"),
    "euGraveShare": ("p1", "p1_grave"),
    "karla": ("p2", "p2"),
    "marcos": ("p3", "p3"),
    "quartoshare": ("bg", "quarto"),
    "sala": (None, "sala"),
    "salasusurros": ("bg", "sala_sussurros"),
}


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "dataset/raw/manifest.csv"
    rows = list(csv.DictReader(open(path, encoding="utf-8")))
    fields = list(rows[0].keys())
    changed = 0
    for r in rows:
        if r["speaker"] in ("bg", "teste"):
            if r["env"] in MAP:
                r["env"] = MAP[r["env"]][1]
            continue
        if r["env"] in MAP and MAP[r["env"]][0]:
            r["speaker"], r["env"] = MAP[r["env"]]
            changed += 1
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    print(f"{changed} linhas reatribuídas em {path}")


if __name__ == "__main__":
    main()
