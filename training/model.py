"""CNN pequena para reconhecimento de palavras sobre features (61 frames x 15).

A mesma arquitetura é reimplementada à mão em C
(firmware/libraries/kws_common/src/kws_model.c) a partir dos pesos do .onnx.
Se mudar algo aqui, o export_c.py recusa o grafo até o C ser atualizado.
"""
import torch
import torch.nn as nn

from labels import CLASSES, TARGETS  # noqa: F401  (re-exportados)


class KwsCNN(nn.Module):
    def __init__(self, n_frames=61, n_feat=15, mean=None, std=None):
        super().__init__()
        # Normalização por feature (média/desvio do conjunto de treino) — vai para o ONNX.
        self.register_buffer("mean", torch.zeros(1, 1, 1, n_feat) if mean is None else mean.view(1, 1, 1, -1))
        self.register_buffer("std", torch.ones(1, 1, 1, n_feat) if std is None else std.view(1, 1, 1, -1))
        self.conv1 = nn.Conv2d(1, 8, kernel_size=3, padding=1)
        self.conv2 = nn.Conv2d(8, 16, kernel_size=3, padding=1)
        self.pool = nn.MaxPool2d(2)
        h, w = n_frames // 2 // 2, n_feat // 2 // 2
        self.fc1 = nn.Linear(16 * h * w, 32)
        self.fc2 = nn.Linear(32, len(CLASSES))
        self.drop = nn.Dropout(0.3)

    def forward(self, x, with_softmax=True):
        # x: (batch, 1, frames, feats)
        x = (x - self.mean) / self.std
        x = self.pool(torch.relu(self.conv1(x)))
        x = self.pool(torch.relu(self.conv2(x)))
        x = torch.flatten(x, 1)
        x = self.drop(torch.relu(self.fc1(x)))
        x = self.fc2(x)
        return torch.softmax(x, dim=1) if with_softmax else x


def count_params(model):
    return sum(p.numel() for p in model.parameters())


def count_macs(n_frames=61, n_feat=15):
    c1 = n_frames * n_feat * 8 * 9
    h, w = n_frames // 2, n_feat // 2
    c2 = h * w * 16 * 8 * 9
    h2, w2 = h // 2, w // 2
    f1 = 16 * h2 * w2 * 32
    f2 = 32 * len(CLASSES)
    return c1 + c2 + f1 + f2
