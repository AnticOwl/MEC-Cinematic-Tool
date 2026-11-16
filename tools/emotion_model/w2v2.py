# audeering's wav2vec2-large-robust-12 emotion model (arousal, dominance, valence), written out in plain torch from
# Hugging Face's Wav2Vec2 code (the "stable layer norm" variant, config.json: do_stable_layer_norm true,
# feat_extract_norm "layer") and audeering's EmotionModel (model card: wav2vec2 -> mean over time -> RegressionHead
# dense 1024->1024, tanh, out_proj 1024->3). No transformers package: the weights are read straight from
# model.safetensors with numpy.
#
# Credit: this follows the Wav2Vec2 model code of Hugging Face Transformers
# (https://github.com/huggingface/transformers; Apache License 2.0, https://www.apache.org/licenses/LICENSE-2.0),
# rewritten here in plain torch for this one configuration, and audeering's model card
# (https://huggingface.co/audeering/wav2vec2-large-robust-12-ft-emotion-msp-dim).
#
# The whole network runs on ONE signal with the time axis as rows, (T, C), so every nn.Linear exports as an ONNX Gemm
# on the ORIGINAL weight (transB = 1) - nothing transposed or folded into new tensors. That lets the ONNX file leave the
# weights out and point into the user's own model.safetensors (external data), see make_onnx.py.
#
# The input is the raw 16 kHz signal; the processor's normalisation (zero mean, unit variance, + 1e-7, numpy's
# population variance: Wav2Vec2FeatureExtractor.zero_mean_unit_var_norm) is done inside the network.
import json, struct
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F


def read_safetensors(path):
    """name -> (numpy array view, absolute byte offset in the file, byte length). Everything here is F32."""
    with open(path, 'rb') as f:
        n = struct.unpack('<Q', f.read(8))[0]
        head = json.loads(f.read(n))
    data_start = 8 + n
    mm = np.memmap(path, dtype=np.uint8, mode='r')
    out = {}
    for name, info in head.items():
        if name == '__metadata__':
            continue
        assert info['dtype'] == 'F32', (name, info['dtype'])
        a, b = info['data_offsets']
        arr = np.frombuffer(mm[data_start + a:data_start + b], dtype='<f4').reshape(info['shape'])
        out[name] = (arr, data_start + a, b - a)
    return out


class ConvLayer(nn.Module):  # Wav2Vec2LayerNormConvLayer: conv -> layer norm over channels -> gelu
    def __init__(self, cin, k, s):
        super().__init__()
        self.conv = nn.Conv1d(cin, 512, k, s, bias=True)
        self.layer_norm = nn.LayerNorm(512, eps=1e-5)
        self.k, self.s = k, s

    def forward(self, x):  # (1, C, T)
        x = self.conv(x)
        x = self.layer_norm(x.transpose(1, 2)).transpose(1, 2)
        return F.gelu(x)


class FeatureExtractor(nn.Module):
    def __init__(self):
        super().__init__()
        ks = [10, 3, 3, 3, 3, 2, 2]
        st = [5, 2, 2, 2, 2, 2, 2]
        self.conv_layers = nn.ModuleList([ConvLayer(1 if i == 0 else 512, k, s) for i, (k, s) in enumerate(zip(ks, st))])

    def forward(self, wav):  # (1, N) -> (T, 512)
        x = wav.unsqueeze(1)
        for c in self.conv_layers:
            x = c(x)
        return x[0].transpose(0, 1)


class FeatureProjection(nn.Module):
    def __init__(self):
        super().__init__()
        self.layer_norm = nn.LayerNorm(512, eps=1e-5)
        self.projection = nn.Linear(512, 1024)

    def forward(self, x):
        return self.projection(self.layer_norm(x))


class PosConv(nn.Module):  # conv with weight norm over dim 2 (weight_g [1,1,128]), then drop the last frame, gelu
    def __init__(self):
        super().__init__()
        self.weight_g = nn.Parameter(torch.zeros(1, 1, 128))
        self.weight_v = nn.Parameter(torch.zeros(1024, 64, 128))
        self.bias = nn.Parameter(torch.zeros(1024))


class PosConvEmbed(nn.Module):
    def __init__(self):
        super().__init__()
        self.conv = PosConv()

    def forward(self, h):  # (T, 1024) -> (T, 1024)
        v, g = self.conv.weight_v, self.conv.weight_g
        w = v * (g / torch.sqrt((v * v).sum(dim=(0, 1), keepdim=True)))
        x = F.conv1d(h.transpose(0, 1).unsqueeze(0), w, self.conv.bias, padding=64, groups=16)
        x = x[:, :, :-1]  # Wav2Vec2SamePadLayer: kernel 128 is even, one frame too many at the end
        return F.gelu(x)[0].transpose(0, 1)


class Attention(nn.Module):
    def __init__(self):
        super().__init__()
        self.q_proj = nn.Linear(1024, 1024)
        self.k_proj = nn.Linear(1024, 1024)
        self.v_proj = nn.Linear(1024, 1024)
        self.out_proj = nn.Linear(1024, 1024)

    def forward(self, h):  # (T, 1024)
        q = self.q_proj(h) * (64 ** -0.5)
        k = self.k_proj(h)
        v = self.v_proj(h)
        q = q.reshape(-1, 16, 64).transpose(0, 1)  # (16, T, 64)
        k = k.reshape(-1, 16, 64).transpose(0, 1)
        v = v.reshape(-1, 16, 64).transpose(0, 1)
        a = torch.softmax(torch.matmul(q, k.transpose(1, 2)), dim=-1)
        o = torch.matmul(a, v).transpose(0, 1).reshape(-1, 1024)
        return self.out_proj(o)


class FeedForward(nn.Module):
    def __init__(self):
        super().__init__()
        self.intermediate_dense = nn.Linear(1024, 4096)
        self.output_dense = nn.Linear(4096, 1024)

    def forward(self, h):
        return self.output_dense(F.gelu(self.intermediate_dense(h)))


class Layer(nn.Module):  # Wav2Vec2EncoderLayerStableLayerNorm (pre-norm)
    def __init__(self):
        super().__init__()
        self.layer_norm = nn.LayerNorm(1024, eps=1e-5)
        self.attention = Attention()
        self.final_layer_norm = nn.LayerNorm(1024, eps=1e-5)
        self.feed_forward = FeedForward()

    def forward(self, h):
        h = h + self.attention(self.layer_norm(h))
        return h + self.feed_forward(self.final_layer_norm(h))


class Encoder(nn.Module):  # Wav2Vec2EncoderStableLayerNorm
    def __init__(self, layers=12):
        super().__init__()
        self.pos_conv_embed = PosConvEmbed()
        self.layers = nn.ModuleList([Layer() for _ in range(layers)])
        self.layer_norm = nn.LayerNorm(1024, eps=1e-5)

    def forward(self, h):
        h = h + self.pos_conv_embed(h)
        for l in self.layers:
            h = l(h)
        return self.layer_norm(h)


class Wav2Vec2(nn.Module):
    def __init__(self):
        super().__init__()
        self.feature_extractor = FeatureExtractor()
        self.feature_projection = FeatureProjection()
        self.encoder = Encoder()

    def forward(self, wav):
        return self.encoder(self.feature_projection(self.feature_extractor(wav)))


class Head(nn.Module):  # audeering's RegressionHead
    def __init__(self):
        super().__init__()
        self.dense = nn.Linear(1024, 1024)
        self.out_proj = nn.Linear(1024, 3)

    def forward(self, x):
        return self.out_proj(torch.tanh(self.dense(x)))


class FeelHead(nn.Module):  # train_head.py's logistic regression, its standardisation folded in: (1, 1024) -> (1, 8)
    def __init__(self):
        super().__init__()
        self.linear = nn.Linear(1024, 8)

    def set_from(self, head_npz):
        h = np.load(head_npz)
        w = h['W'] / h['sd'][:, None]                  # (1024, 8)
        b = h['b'] - (h['mu'] / h['sd']) @ h['W']
        with torch.no_grad():
            self.linear.weight.copy_(torch.from_numpy(w.T.astype(np.float32)))
            self.linear.bias.copy_(torch.from_numpy(b.astype(np.float32)))
        return [str(c) for c in h['classes']]

    def forward(self, e):
        return torch.softmax(self.linear(e), dim=-1)


class EmotionModel(nn.Module):
    def __init__(self, feel=False):
        super().__init__()
        self.wav2vec2 = Wav2Vec2()
        self.classifier = Head()
        self.feel = FeelHead() if feel else None

    def forward(self, wav):  # (1, N) raw 16 kHz -> (1, 3) arousal, dominance, valence [, (1, 8) feeling probabilities]
        m = wav.mean(dim=1, keepdim=True)
        d = wav - m
        wav = d / torch.sqrt((d * d).mean(dim=1, keepdim=True) + 1e-7)
        e = self.wav2vec2(wav).mean(dim=0, keepdim=True)
        if self.feel is None:
            return self.classifier(e)
        return self.classifier(e), self.feel(e)

    def embed(self, wav):  # the pooled hidden states (1, 1024) and the outputs (1, 3)
        m = wav.mean(dim=1, keepdim=True)
        d = wav - m
        wav = d / torch.sqrt((d * d).mean(dim=1, keepdim=True) + 1e-7)
        e = self.wav2vec2(wav).mean(dim=0, keepdim=True)
        return e, self.classifier(e)


def load(path, head_npz=None):
    st = read_safetensors(path)
    m = EmotionModel(feel=head_npz is not None).eval()
    if head_npz is not None:
        m.feel.set_from(head_npz)
    sd = m.state_dict()
    used = set()
    with torch.no_grad():
        for name, p in sd.items():
            if name.startswith('feel.'):
                continue
            arr = st[name][0]
            assert tuple(arr.shape) == tuple(p.shape), (name, arr.shape, p.shape)
            p.copy_(torch.from_numpy(np.array(arr)))
            used.add(name)
    left = sorted(set(st) - used)
    return m, st, left
