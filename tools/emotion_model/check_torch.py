# Loads the weights into w2v2.py's network and checks it against the model card's own reference:
# 1 s of zeros at 16 kHz -> [[0.5460754 0.6062266 0.40431657]] (arousal, dominance, valence).
# Then writes reference inputs/outputs for the ONNX checks (raw float32 files + refs.json).
# Usage: python -I check_torch.py <model.safetensors> <out dir>
import ctypes, json, os, sys, time
ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x40)  # IDLE_PRIORITY_CLASS
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import torch
from w2v2 import load

torch.set_num_threads(4)
st_path, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
t = time.perf_counter()
m, st, left = load(st_path)
print(f'loaded in {time.perf_counter() - t:.1f} s; safetensors tensors not used: {left}')

card = np.array([0.5460754, 0.6062266, 0.40431657])
with torch.no_grad():
    y = m(torch.zeros(1, 16000)).numpy().ravel()
print('zeros 1 s :', y, ' card:', card, ' max diff', np.abs(y - card).max())

rng = np.random.default_rng(7)
cases = {}
sr = 16000
tt = np.arange(sr * 3) / sr
cases['sine_3s'] = (0.3 * np.sin(2 * np.pi * 150 * tt) + 0.05 * np.sin(2 * np.pi * 2300 * tt)).astype(np.float32)
for secs in (0.5, 1.37, 4.0, 6.0):
    cases[f'noise_{secs}s'] = (rng.normal(0, 0.1, int(secs * sr))).astype(np.float32)
# a voiced-like test: a gliding harmonic tone with an envelope (still synthetic)
n = int(2.2 * sr)
f0 = 120 + 80 * np.sin(2 * np.pi * 0.7 * np.arange(n) / sr)
ph = 2 * np.pi * np.cumsum(f0) / sr
sig = sum(np.sin(k * ph) / k for k in range(1, 12)) * (0.5 + 0.5 * np.sin(2 * np.pi * 3 * np.arange(n) / sr) ** 2)
cases['glide_2.2s'] = (0.2 * sig).astype(np.float32)
cases['zeros_1s'] = np.zeros(sr, np.float32)

refs = {}
for name, x in cases.items():
    x.tofile(os.path.join(out, name + '.f32'))
    with torch.no_grad():
        t = time.perf_counter()
        y = m(torch.from_numpy(x).unsqueeze(0)).numpy().ravel()
        dt = time.perf_counter() - t
    refs[name] = {'samples': int(x.size), 'adv': [float(v) for v in y]}
    print(f'{name:12s} {x.size:7d} samples  A {y[0]:.6f} D {y[1]:.6f} V {y[2]:.6f}  ({dt * 1000:.0f} ms)')
json.dump(refs, open(os.path.join(out, 'refs.json'), 'w'), indent=1)
