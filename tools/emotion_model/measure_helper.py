# The shipped path measured: EmotionHelper.exe (Windows ML, the ONNX file that is installed) on every EmoDB and RAVDESS
# recording. Recordings are joined into job files of at most 590 s (0.5 s of silence between them) with one stretch per
# recording (heard exactly as it is). Then the tools' rule (voice_emotion::LookOf at the default sureness 0.55) gives
# what the face would show. Also: how often the helper's top feeling equals PyTorch's (embeddings.npz + head.npz).
# RAVDESS here is the head's own training data (in-sample: optimistic); its honest figure is leave-one-actor-out
# (mapping_eval.py). EmoDB was never used to fit anything.
# Usage: python -I measure_helper.py <helper dir> <work dir> <emodb wav dir> <ravdess dir> <embeddings.npz> <head.npz>
import ctypes, os, struct, subprocess, sys, time, wave
ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x40)
import numpy as np

hdir, work, emodb, rav, emb_path, head_path = sys.argv[1:7]
os.makedirs(work, exist_ok=True)
exe = os.path.join(hdir, 'EmotionHelper.exe')
CL = ['neutral', 'calm', 'happy', 'sad', 'angry', 'fearful', 'disgust', 'surprised']
EMO_E = {'W': 'angry', 'L': 'calm', 'E': 'disgust', 'A': 'fearful', 'F': 'happy', 'T': 'sad', 'N': 'neutral'}
EMO_R = {'01': 'neutral', '02': 'calm', '03': 'happy', '04': 'sad', '05': 'angry', '06': 'fearful', '07': 'disgust',
         '08': 'surprised'}


def read(p):
    with wave.open(p, 'rb') as w:
        assert w.getsampwidth() == 2
        x = np.frombuffer(w.readframes(w.getnframes()), '<i2').reshape(-1, w.getnchannels())
        return x[:, 0] if x.shape[1] == 1 else x.mean(1).astype(np.int16), w.getframerate()


files = [(os.path.join(emodb, f), 'emodb', EMO_E[f[5]]) for f in sorted(os.listdir(emodb)) if f.endswith('.wav')]
for d in sorted(os.listdir(rav)):
    for f in sorted(os.listdir(os.path.join(rav, d))):
        if f.endswith('.wav'):
            files.append((os.path.join(rav, d, f), 'ravdess', EMO_R[f.split('-')[2]]))
# jobs by rate
results = {}
t0 = time.perf_counter()
for corpus in ('emodb', 'ravdess'):
    group = [f for f in files if f[1] == corpus]
    chunk, rate, pos, idx, jobs = [], None, 0, [], []
    def flush():
        global chunk, pos, idx
        if chunk:
            jobs.append((np.concatenate(chunk), rate, idx))
        chunk, pos, idx = [], 0, []
    for p, c, lab in group:
        x, r = read(p)
        rate = r if rate is None else rate
        assert r == rate
        if (pos + len(x)) / r > 590:
            flush()
        idx.append((p, lab, pos / r, (pos + len(x)) / r))
        chunk.append(x)
        chunk.append(np.zeros(int(0.5 * r), np.int16))
        pos += len(x) + int(0.5 * r)
    flush()
    for j, (pcm, r, items) in enumerate(jobs):
        job = os.path.join(work, f'{corpus}_{j}.job')
        res = os.path.join(work, f'{corpus}_{j}.txt')
        b = b'MECEMO1\0' + struct.pack('<IIQ', r, 1, len(pcm)) + pcm.astype('<i2').tobytes()
        b += struct.pack('<I', len(items)) + b''.join(struct.pack('<dd', a, e) for _, _, a, e in items)
        open(job, 'wb').write(b)
        if os.path.exists(res):
            os.remove(res)
        ts = time.perf_counter()
        rc = subprocess.run([exe, job, res], creationflags=0x40).returncode
        lines = open(res).read().strip().split('\n')
        assert rc == 0 and lines[0] == f'MECEMO1 ok {len(items)}', (rc, lines[0])
        for (p, lab, a, e), line in zip(items, lines[1:]):
            v = [float(s) for s in line.split()[1:]]
            results[p] = (corpus, lab, np.array(v))
        if os.path.exists(job):   # (the helper deletes its job file once read)
            os.remove(job)
        print(f'{corpus} job {j}: {len(items)} recordings, {len(pcm) / r:.0f} s of sound, {time.perf_counter() - ts:.0f} s', flush=True)
print(f'all {len(results)} recordings in {time.perf_counter() - t0:.0f} s', flush=True)


def smooth(lo, hi, p):
    x = np.clip((p - lo) / (hi - lo), 0, 1)
    return x * x * (3 - 2 * x)


# PyTorch's chances for the same recordings (embeddings.npz is in the same order: EmoDB sorted, then RAVDESS sorted)
d = np.load(emb_path)
h = np.load(head_path)
z = ((d['emb'] - h['mu']) / h['sd']) @ h['W'] + h['b']
z -= z.max(1, keepdims=True)
P_torch = np.exp(z) / np.exp(z).sum(1, keepdims=True)
keys = [f[0] for f in files]
P_helper = np.array([results[k][2][3:] for k in keys])
same = (P_torch.argmax(1) == P_helper.argmax(1))
print(f'top feeling, helper vs PyTorch: the same in {same.sum()} of {len(same)}; largest chance difference '
      f'{np.abs(P_torch - P_helper).max():.4f}')
for corpus in ('emodb', 'ravdess'):
    k = np.array([results[x][0] == corpus for x in keys])
    labs = np.array([results[x][1] for x in keys])[k]
    P = P_helper[k]
    top = P.argmax(1)
    acc = np.mean([CL[t] == l for t, l in zip(top, labs)])
    share = smooth(0.55, 0.95, P.max(1))
    share[top == 0] = 0
    print(f'\n== {corpus} through the helper{" (its training data: in-sample)" if corpus == "ravdess" else ""}: '
          f'top feeling right {acc:.3f}')
    print('true        right  wrong   base')
    tot = np.zeros(3)
    n = 0
    for c in CL:
        kk = labs == c
        if not kk.any():
            continue
        right = np.where(np.array([CL[t] == c for t in top[kk]]), share[kk], 0.0)
        if c == 'neutral':
            v = ((1 - share[kk]).mean(), share[kk].mean(), 0.0)
        else:
            v = (right.mean(), (share[kk] - right).mean(), (1 - share[kk]).mean())
        tot += v
        n += 1
        print(f'{c:10s} {v[0]:6.2f} {v[1]:6.2f} {v[2]:6.2f}')
    print(f'mean       {tot[0] / n:6.2f} {tot[1] / n:6.2f} {tot[2] / n:6.2f}')
