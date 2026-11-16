# Pooled 1024-number summaries (and A/D/V) of every EmoDB and RAVDESS recording, saved to one .npz, which
# train_head.py trains and tests the 8-feeling head on (and measure_helper.py / mapping_eval.py read).
# Usage: python -I embed_corpora.py <model.safetensors> <emodb wav dir> <ravdess dir> <out.npz>
import ctypes, os, sys, wave
ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x40)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import torch
import torch.nn.functional as F
from w2v2 import load
import resample

st_path, emodb, rav, out = sys.argv[1:5]
dev = 'cuda' if torch.cuda.is_available() else 'cpu'
m, st, left = load(st_path)
m = m.to(dev)
cut = 0.5 * 16000 * resample.ROLLOFF
step = 48000 / (2 * cut)
half = resample.ZEROS * step
J = np.arange(-int(np.floor(half)), int(np.floor(half)) + 1)
u = J / half
h = (2 * cut / 48000) * np.sinc(J / step) * np.where(np.abs(u) < 1, np.i0(resample.BETA * np.sqrt(np.clip(1 - u * u, 0, 1))) / np.i0(resample.BETA), 0)
hk = torch.tensor(h[::-1].copy(), dtype=torch.float64).view(1, 1, -1)
P = int(np.floor(half))


def read(p):
    with wave.open(p, 'rb') as w:
        ch, sw, fr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        x = np.frombuffer(w.readframes(n), '<i2').astype(np.float32).reshape(-1, ch).mean(1) / 32768.0
    if fr == 48000:
        xt = torch.from_numpy(np.asarray(x, np.float64)).view(1, 1, -1)
        y = F.conv1d(F.pad(xt, (P, P)), hk)[0, 0].numpy()
        return y[0:3 * (len(x) // 3):3].astype(np.float32)
    assert fr == 16000
    return x


rows = []
EMO_E = {'W': 'angry', 'L': 'calm', 'E': 'disgust', 'A': 'fearful', 'F': 'happy', 'T': 'sad', 'N': 'neutral'}
EMO_R = {'01': 'neutral', '02': 'calm', '03': 'happy', '04': 'sad', '05': 'angry', '06': 'fearful', '07': 'disgust',
         '08': 'surprised'}
embs, advs, corpus, label, speaker, intensity = [], [], [], [], [], []
for fn in sorted(os.listdir(emodb)):
    if fn.lower().endswith('.wav'):
        x = read(os.path.join(emodb, fn))
        with torch.no_grad():
            e, a = m.embed(torch.from_numpy(x).unsqueeze(0).to(dev))
        embs.append(e.cpu().numpy().ravel()); advs.append(a.cpu().numpy().ravel())
        corpus.append('emodb'); label.append(EMO_E[fn[5]]); speaker.append('e' + fn[0:2]); intensity.append('-')
for d in sorted(os.listdir(rav)):
    for fn in sorted(os.listdir(os.path.join(rav, d))):
        if fn.lower().endswith('.wav'):
            x = read(os.path.join(rav, d, fn))
            with torch.no_grad():
                e, a = m.embed(torch.from_numpy(x).unsqueeze(0).to(dev))
            f = fn[:-4].split('-')
            embs.append(e.cpu().numpy().ravel()); advs.append(a.cpu().numpy().ravel())
            corpus.append('ravdess'); label.append(EMO_R[f[2]]); speaker.append('r' + f[6]); intensity.append(f[3])
np.savez(out, emb=np.array(embs), adv=np.array(advs), corpus=np.array(corpus), label=np.array(label),
         speaker=np.array(speaker), intensity=np.array(intensity))
print(len(embs), 'recordings saved')
