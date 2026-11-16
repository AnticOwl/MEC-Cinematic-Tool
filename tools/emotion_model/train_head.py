# The feeling head: multinomial logistic regression on the model's pooled 1024-number summary, 8 RAVDESS emotions
# (neutral calm happy sad angry fearful disgust surprised), trained on RAVDESS ONLY. EmoDB (German, other speakers,
# other sentences) is never used to fit anything: it is the independent test.
#  1. L2 strength chosen by leave-one-actor-out inside RAVDESS.
#  2. Reported: RAVDESS leave-one-actor-out (8 classes), and the head trained on all RAVDESS tested on EmoDB
#     (boredom counted as calm; EmoDB has no "surprised").
#  3. How often the top answer is right at each confidence (top probability) on EmoDB and in leave-one-actor-out.
#  4. Saved: head.npz (mean, sd, W, b, class names) for the ONNX graph.
# Usage: python -I train_head.py <embeddings.npz> <out head.npz>
import sys
import numpy as np
import torch

d = np.load(sys.argv[1])
emb, corpus, label, speaker = d['emb'], d['corpus'], d['label'], d['speaker']
CLASSES = ['neutral', 'calm', 'happy', 'sad', 'angry', 'fearful', 'disgust', 'surprised']


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


def report(title, ytrue, P, classes_true):
    pred = P.argmax(1)
    conf = np.zeros((len(classes_true), len(CLASSES)), int)
    for t, p in zip(ytrue, pred):
        conf[classes_true.index(t), p] += 1
    right = np.array([CLASSES[p] == t for t, p in zip(ytrue, pred)])
    rec = np.array([right[ytrue == c].mean() for c in classes_true])
    print(f'\n== {title}: accuracy {right.mean():.3f}, mean recall {rec.mean():.3f}')
    print('true \\ said  ' + ' '.join(f'{c[:7]:>7s}' for c in CLASSES) + '   recall')
    for i, c in enumerate(classes_true):
        print(f'{c[:12]:12s} ' + ' '.join(f'{v:7d}' for v in conf[i]) + f'   {rec[i]:.2f}')
    top = P.max(1)
    print('  top probability:   ' + '  '.join(f'{lo:.2f}-{min(hi, 1.0):.2f}' for lo, hi in BANDS))
    print('  share of answers:  ' + '  '.join(f'{((top >= lo) & (top < hi)).mean():7.2f}' for lo, hi in BANDS))
    print('  right in band:     ' + '  '.join(f'{right[(top >= lo) & (top < hi)].mean() if ((top >= lo) & (top < hi)).any() else float("nan"):7.2f}' for lo, hi in BANDS))
    print('  at this chance or more (share of answers, right):  ' + '   '.join(f'>= {t:.2f}: {(top >= t).mean():.2f}, {right[top >= t].mean():.3f}' for t in (0.8, 0.9, 0.95)))
    return right, top


BANDS = [(0.0, 0.4), (0.4, 0.6), (0.6, 0.8), (0.8, 0.9), (0.9, 0.95), (0.95, 1.01)]
kr = corpus == 'ravdess'
ke = corpus == 'emodb'
Xr, yr, sr = emb[kr], label[kr], speaker[kr]
best = None
for l2 in (0.003, 0.01, 0.03, 0.1, 0.3):
    P = np.zeros((len(yr), len(CLASSES)))
    for s in sorted(set(sr)):
        P[sr == s] = probs(train(Xr[sr != s], yr[sr != s], l2), Xr[sr == s])
    acc = np.mean([CLASSES[p] == t for t, p in zip(yr, P.argmax(1))])
    ll = -np.mean(np.log(P[np.arange(len(yr)), [CLASSES.index(c) for c in yr]] + 1e-12))
    print(f'l2 {l2}: RAVDESS leave-one-actor-out accuracy {acc:.3f}, log loss {ll:.3f}')
    if best is None or ll < best[1]:
        best = (l2, ll, P)
l2, _, Ploao = best
print(f'chosen l2 {l2} (lowest leave-one-actor-out log loss)')
report(f'RAVDESS leave-one-actor-out, 8 classes, l2 {l2}', yr, Ploao, CLASSES)
head = train(Xr, yr, l2)
ye = np.array(['calm' if c == 'calm' else c for c in label[ke]])  # EmoDB boredom was already named calm
Pe = probs(head, emb[ke])
report('trained on all RAVDESS, tested on EmoDB (boredom = calm)', ye, Pe, [c for c in CLASSES if c in set(ye)])
mu, sd, W, b = head
np.savez(sys.argv[2], mu=mu, sd=sd, W=W, b=b, classes=np.array(CLASSES), l2=l2)
print('saved', sys.argv[2])
