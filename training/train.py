#!/usr/bin/env python3
"""Treina o classificador de palavras, avalia por pessoa e exporta o .onnx.

    python3 training/train.py --root dataset/raw --out model [--ablation]

Etapas:
  1) extrai features de todos os clipes (mesmo código/regra do ESP32);
  2) validação cruzada "leave-one-speaker-out": para cada pessoa, treina com as
     outras e testa nela (voz nunca vista) -> acurácia honesta;
  3) escolhe o limiar de confiança (threshold) nas predições da validação cruzada;
  4) treina o modelo final com todas as pessoas e exporta model/kws.onnx.
"""
import argparse
import datetime as dt
import json
import os
import sys
import time

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import features as F  # noqa: E402
from dataset import augmented_windows, eval_window, load_manifest, read_wav  # noqa: E402
from model import CLASSES, TARGETS, KwsCNN, count_macs, count_params  # noqa: E402

DECISIONS = TARGETS + ["unknown"]  # saída final do sistema (noise vira unknown)


# --------------------------------------------------------------------------- dados
def build_windows(clips, n_aug, seed):
    rng = np.random.default_rng(seed)
    bg_pool = [read_wav(c["path"]) for c in clips if c["speaker"] == "bg"]
    data = []
    no_vad = 0
    t0 = time.time()
    for i, c in enumerate(clips):
        x = read_wav(c["path"])
        feats = F.clip_features(x)
        w, fired = eval_window(feats, c["label"])
        if c["label"] != "noise" and not fired:
            no_vad += 1
        aug = augmented_windows(x, c["label"], rng, n_aug, bg_pool)
        data.append({"clip": c, "eval": w, "aug": np.stack(aug) if aug else np.zeros((0, *w.shape), np.float32),
                     "y": CLASSES.index(c["label"]), "vad": fired})
        if (i + 1) % 200 == 0:
            print(f"  features: {i + 1}/{len(clips)} clipes ({time.time() - t0:.0f} s)")
    print(f"  fala sem disparo do VAD (usado fallback): {no_vad} clipes")
    return data


def stack(items, use_aug):
    X, y = [], []
    for d in items:
        X.append(d["eval"][None])
        y.append(d["y"])
        if use_aug:
            X.append(d["aug"])
            y += [d["y"]] * len(d["aug"])
    return np.concatenate(X).astype(np.float32), np.array(y)


# --------------------------------------------------------------------------- treino
def train_model(X, y, n_feat, epochs, seed, log=False):
    torch.manual_seed(seed)
    Xt = torch.from_numpy(X[:, None, :, :n_feat])
    yt = torch.from_numpy(y).long()
    mean = Xt.mean(dim=(0, 1, 2))
    std = Xt.std(dim=(0, 1, 2)) + 1e-3
    model = KwsCNN(n_frames=F.WIN_FRAMES, n_feat=n_feat, mean=mean, std=std)
    counts = np.bincount(y, minlength=len(CLASSES)).astype(np.float32)
    weights = torch.tensor(counts.sum() / np.maximum(counts, 1) / len(CLASSES))
    loss_fn = nn.CrossEntropyLoss(weight=weights)
    opt = torch.optim.Adam(model.parameters(), lr=2e-3, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)
    g = torch.Generator().manual_seed(seed)
    for ep in range(epochs):
        model.train()
        perm = torch.randperm(len(Xt), generator=g)
        tot = 0.0
        for i in range(0, len(perm), 64):
            idx = perm[i:i + 64]
            opt.zero_grad()
            loss = loss_fn(model(Xt[idx], with_softmax=False), yt[idx])
            loss.backward()
            opt.step()
            tot += loss.item() * len(idx)
        sched.step()
        if log and (ep % 5 == 4 or ep == epochs - 1):
            print(f"    época {ep + 1:3d}/{epochs}  loss {tot / len(Xt):.4f}")
    model.eval()
    return model


def predict(model, X, n_feat):
    with torch.no_grad():
        return model(torch.from_numpy(X[:, None, :, :n_feat].astype(np.float32))).numpy()


# --------------------------------------------------------------------------- decisão e métricas
def decide(probs, tau):
    """Regra de decisão do ESP32: palavra-alvo só se a confiança >= tau; o resto é unknown."""
    k = probs.argmax(axis=1)
    out = []
    for i, kk in enumerate(k):
        c = CLASSES[kk]
        out.append(c if c in TARGETS and probs[i, kk] >= tau else "unknown")
    return np.array(out)


def truth4(y):
    return np.array([CLASSES[i] if CLASSES[i] in TARGETS else "unknown" for i in y])


def metrics4(y_true4, y_pred4):
    labels = DECISIONS
    cm = np.zeros((len(labels), len(labels)), int)
    for t, p in zip(y_true4, y_pred4):
        cm[labels.index(t), labels.index(p)] += 1
    f1s = []
    for i in range(len(labels)):
        tp = cm[i, i]
        prec = tp / max(cm[:, i].sum(), 1)
        rec = tp / max(cm[i].sum(), 1)
        f1s.append(0 if prec + rec == 0 else 2 * prec * rec / (prec + rec))
    non_target = cm[3].sum()
    false_accept = cm[3, :3].sum() / max(non_target, 1)  # "unknown" aceito como palavra
    target_rows = cm[:3]
    target_recall = np.trace(cm[:3, :3]) / max(target_rows.sum(), 1)
    return {"accuracy": float(np.trace(cm) / max(cm.sum(), 1)), "macro_f1": float(np.mean(f1s)),
            "false_accept_rate": float(false_accept), "target_recall": float(target_recall),
            "confusion": cm.tolist(), "labels": labels}


def choose_tau(probs, y):
    best = None
    for tau in np.round(np.arange(0.30, 0.96, 0.05), 2):
        m = metrics4(truth4(y), decide(probs, tau))
        score = m["macro_f1"]
        if best is None or score > best[1] + 1e-9:
            best = (float(tau), score, m)
    return best


def confusion5(y, probs):
    cm = np.zeros((5, 5), int)
    for t, p in zip(y, probs.argmax(axis=1)):
        cm[t, p] += 1
    return cm


def md_table(cm, labels):
    s = "| real \\ previsto | " + " | ".join(labels) + " |\n|" + "---|" * (len(labels) + 1) + "\n"
    for i, l in enumerate(labels):
        s += f"| **{l}** | " + " | ".join(str(v) for v in cm[i]) + " |\n"
    return s


# --------------------------------------------------------------------------- validação cruzada
def cross_validate(data, n_feat, epochs, seed):
    speakers = sorted({d["clip"]["speaker"] for d in data if d["clip"]["speaker"] != "bg"})
    bg = [d for d in data if d["clip"]["speaker"] == "bg"]
    oof_p, oof_y, per_speaker = [], [], {}
    for s in speakers:
        tr = [d for d in data if d["clip"]["speaker"] not in (s, "bg")] + bg
        te = [d for d in data if d["clip"]["speaker"] == s]
        Xtr, ytr = stack(tr, use_aug=True)
        Xte, yte = stack(te, use_aug=False)
        model = train_model(Xtr, ytr, n_feat, epochs, seed)
        p = predict(model, Xte, n_feat)
        acc5 = float((p.argmax(1) == yte).mean())
        per_speaker[s] = {"n_test": int(len(yte)), "acc5": acc5}
        print(f"  pessoa {s}: treino={len(ytr)} janelas, teste={len(yte)} clipes, acurácia 5 classes={acc5:.3f}")
        oof_p.append(p)
        oof_y.append(yte)
    return np.concatenate(oof_p), np.concatenate(oof_y), per_speaker


# --------------------------------------------------------------------------- export
def export_onnx(model, n_feat, path):
    class WithSoftmax(nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, x):
            return self.m(x, with_softmax=True)

    dummy = torch.zeros(1, 1, F.WIN_FRAMES, n_feat)
    torch.onnx.export(WithSoftmax(model).eval(), dummy, path, input_names=["features"], output_names=["probs"],
                      opset_version=13, do_constant_folding=True, dynamo=False,
                      dynamic_axes={"features": {0: "batch"}, "probs": {0: "batch"}})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="dataset/raw")
    ap.add_argument("--out", default="model")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--aug", type=int, default=6, help="versões aumentadas por clipe")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--ablation", action="store_true", help="compara só-MFCC vs MFCC+RMS+centroid")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    clips = load_manifest(args.root)
    speakers = sorted({c["speaker"] for c in clips if c["speaker"] != "bg"})
    counts = {l: sum(c["label"] == l for c in clips) for l in CLASSES}
    print(f"{len(clips)} clipes, {len(speakers)} pessoas {speakers}, por classe {counts}")
    if len(speakers) < 2:
        sys.exit("preciso de pelo menos 2 pessoas para validar por pessoa")

    print("\n[1] extraindo features...")
    data = build_windows(clips, args.aug, args.seed)

    results = {}
    feat_sets = [("all", F.N_FEAT)] + ([("mfcc", F.N_MFCC)] if args.ablation else [])
    for name, n_feat in feat_sets:
        print(f"\n[2] validação cruzada por pessoa — features={name} ({n_feat})")
        p, y, per_spk = cross_validate(data, n_feat, args.epochs, args.seed)
        tau, _, m4 = choose_tau(p, y)
        results[name] = {"n_feat": n_feat, "acc5": float((p.argmax(1) == y).mean()), "per_speaker": per_spk,
                         "tau": tau, "decision_metrics": m4, "confusion5": confusion5(y, p).tolist()}
        print(f"  => acurácia 5 classes {results[name]['acc5']:.3f} | threshold {tau} | "
              f"decisão 4 vias: acc {m4['accuracy']:.3f}, macro-F1 {m4['macro_f1']:.3f}, "
              f"falso aceite {m4['false_accept_rate']:.3f}, recall palavras {m4['target_recall']:.3f}")

    chosen = results["all"]
    print(f"\n[3] modelo final com todas as pessoas (features=all, threshold={chosen['tau']})")
    Xall, yall = stack(data, use_aug=True)
    model = train_model(Xall, yall, F.N_FEAT, args.epochs, args.seed, log=True)
    onnx_path = os.path.join(args.out, "kws.onnx")
    export_onnx(model, F.N_FEAT, onnx_path)
    print(f"  salvo {onnx_path}  ({os.path.getsize(onnx_path) / 1024:.1f} KB)")

    meta = {
        "created": dt.datetime.now().isoformat(timespec="seconds"),
        "classes": CLASSES, "targets": TARGETS, "threshold": chosen["tau"],
        "input": {"frames": F.WIN_FRAMES, "features": F.N_FEAT, "names": F.FEATURE_NAMES},
        "params": count_params(model), "macs": count_macs(F.WIN_FRAMES, F.N_FEAT),
        "dataset": {"clips": len(clips), "speakers": speakers, "per_class": counts},
        "cv": results, "epochs": args.epochs, "aug": args.aug, "seed": args.seed,
    }
    with open(os.path.join(args.out, "metadata.json"), "w") as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)

    # relatório legível
    r = [f"# Resultados do treino ({meta['created']})\n",
         f"- Clipes: {len(clips)} | pessoas: {len(speakers)} | por classe: {counts}",
         f"- Modelo: {meta['params']} parâmetros, {meta['macs']} MACs por inferência",
         f"- Avaliação: leave-one-speaker-out (cada pessoa testada com modelo que nunca ouviu a voz dela)\n"]
    for name, res in results.items():
        m = res["decision_metrics"]
        r += [f"## Features: {name} ({res['n_feat']} por frame)\n",
              f"- Acurácia 5 classes: **{res['acc5']:.3f}**",
              f"- Threshold escolhido: **{res['tau']}**",
              f"- Decisão final (ball/cat/dog/unknown): acurácia **{m['accuracy']:.3f}**, macro-F1 {m['macro_f1']:.3f}, "
              f"falso aceite {m['false_accept_rate']:.3f}, recall das palavras {m['target_recall']:.3f}\n",
              "Por pessoa:\n", "| pessoa | clipes de teste | acurácia 5 classes |", "|---|---|---|"]
        r += [f"| {s} | {v['n_test']} | {v['acc5']:.3f} |" for s, v in res["per_speaker"].items()]
        r += ["\nMatriz de confusão (5 classes, argmax):\n", md_table(np.array(res["confusion5"]), CLASSES),
              "Matriz de confusão da decisão final (com threshold):\n", md_table(np.array(m["confusion"]), DECISIONS)]
    with open(os.path.join(args.out, "report.md"), "w") as f:
        f.write("\n".join(r))
    print(f"  relatório: {os.path.join(args.out, 'report.md')}")


if __name__ == "__main__":
    main()
