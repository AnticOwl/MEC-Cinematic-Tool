# What the face would show with the tools' rule (voice_emotion::LookOf): the top feeling's share of the face is
# smoothstep(lo, hi, its chance); voice "neutral" (or an unread stretch) leaves the feeling picked by the user ("base").
# For each true emotion: the average share of the face given to the RIGHT preset (neutral: base is right), to WRONG
# presets, and left to base. RAVDESS uses leave-one-actor-out chances (heads that never saw that actor); EmoDB the
# head trained on all of RAVDESS (EmoDB never used to fit). Usage: python -I mapping_eval.py <embeddings.npz> <head.npz>
import sys
import numpy as np
import torch

CLASSES = ['neutral', 'calm', 'happy', 'sad', 'angry', 'fearful', 'disgust', 'surprised']


# train_head.py's own two functions, copied as they are
def train(X, y, l2):
    mu, sd = X.mean(0), X.std(0) + 1e-6
    Xt = torch.tensor((X - mu) / sd, dtype=torch.float64)
    yt = torch.tensor([CLASSES.index(c) for c in y])
    W = torch.zeros(X.shape[1], len(CLASSES), dtype=torch.float64, requires_grad=True)
    b = torch.zeros(len(CLASSES), dtype=torch.float64, requires_grad=True)
    cnt = torch.bincount(yt, minlength=len(CLASSES)).double()
    cw = cnt.sum() / (len(CLASSES) * cnt.clamp(min=1))
    opt = torch.optim.LBFGS([W, b], max_iter=1000, tolerance_grad=1e-9, tolerance_change=1e-12, line_search_fn='strong_wolfe')

    def closure():
        opt.zero_grad()
        loss = torch.nn.functional.cross_entropy(Xt @ W + b, yt, weight=cw) + l2 * (W * W).sum()
        loss.backward()
        return loss
    opt.step(closure)
    return mu, sd, W.detach().numpy(), b.detach().numpy()


def probs(model, X):
    mu, sd, W, b = model
    z = ((X - mu) / sd) @ W + b
    z -= z.max(1, keepdims=True)
    p = np.exp(z)
    return p / p.sum(1, keepdims=True)




class th:
    train = staticmethod(train)
    probs = staticmethod(probs)

d = np.load(sys.argv[1])
h = np.load(sys.argv[2])
CL = [str(c) for c in h['classes']]
emb, corpus, label, speaker = d['emb'], d['corpus'], d['label'], d['speaker']
kr, ke = corpus == 'ravdess', corpus == 'emodb'
l2 = float(h['l2'])
P_r = np.zeros((kr.sum(), len(CL)))
Xr, yr, sr = emb[kr], label[kr], speaker[kr]
for s in sorted(set(sr)):
    P_r[sr == s] = th.probs(th.train(Xr[sr != s], yr[sr != s], l2), Xr[sr == s])
P_e = th.probs((h['mu'], h['sd'], h['W'], h['b']), emb[ke])
y_e = label[ke]


def smooth(lo, hi, p):
    x = np.clip((p - lo) / (hi - lo), 0, 1)
    return x * x * (3 - 2 * x)


def table(P, y, lo, hi, title):
    top = P.argmax(1)
    share = smooth(lo, hi, P.max(1))
    share[np.array([CL[t] == 'neutral' for t in top])] = 0.0
    print(f'\n== {title}, share = smoothstep({lo}, {hi}, chance)')
    print('true        right  wrong   base | most shown wrong')
    tot = np.zeros(3)
    for c in CL:
        k = y == c
        if not k.any():
            continue
        right = np.where(np.array([CL[t] == c for t in top[k]]), share[k], 0.0)
        if c == 'neutral':
            right_v, wrong_v, base_v = (1 - share[k]).mean(), share[k].mean(), (1 - share[k]).mean()
        else:
            right_v, wrong_v, base_v = right.mean(), (share[k] - right).mean(), (1 - share[k]).mean()
        wrong_to = {}
        for t, s in zip(top[k], share[k]):
            if CL[t] != c and s > 0:
                wrong_to[CL[t]] = wrong_to.get(CL[t], 0) + s / k.sum()
        w = sorted(wrong_to.items(), key=lambda kv: -kv[1])[:2]
        print(f'{c:10s} {right_v:6.2f} {wrong_v:6.2f} {base_v if c != "neutral" else 0:6.2f} | ' + ', '.join(f'{a} {b:.2f}' for a, b in w))
        tot += [right_v, wrong_v, base_v if c != 'neutral' else 0]
    n = len([c for c in CL if (y == c).any()])
    print(f'mean       {tot[0] / n:6.2f} {tot[1] / n:6.2f} {tot[2] / n:6.2f}')


# the tools' own rule (voice_emotion::LookOf): from the sureness s to s + 0.4, at most to a chance of 1
for lo, hi in [(s, min(s + 0.4, 1.0)) for s in (0.3, 0.45, 0.55, 0.7, 0.85, 0.95)]:
    table(P_r, yr, lo, hi, 'RAVDESS, actors the head never saw')
    table(P_e, y_e, lo, hi, 'EmoDB (German), never used to fit')
