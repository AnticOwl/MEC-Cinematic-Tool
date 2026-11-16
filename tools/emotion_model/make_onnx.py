# Exports w2v2.py's network to ONNX (TorchScript exporter, no onnx package: it is not installed and must not be),
# opset 17, the sample count free. Two files:
#   <out>\full\emotion_full.onnx      the weights inside (for checking only)
#   <out>\graph\emotion_adv.onnx      NO weights: every weight is "external data" pointing into model.safetensors
#                                     (byte offset + length), which must sit in the same folder. Each one is checked
#                                     byte for byte against the safetensors tensor before it is pointed at.
# With a head.npz (train_head.py) the network also gives "feel": the 8 feeling chances (neutral calm happy sad angry
# fearful disgust surprised); its two small tensors are not in model.safetensors and stay inside both files.
# Usage: python -I make_onnx.py <model.safetensors> <out dir> [opset] [head.npz]
import ctypes, os, sys
ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x40)  # IDLE_PRIORITY_CLASS
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import torch
from w2v2 import load
import pb

st_path, out = sys.argv[1], sys.argv[2]
opset = int(sys.argv[3]) if len(sys.argv) > 3 else 17
head = sys.argv[4] if len(sys.argv) > 4 else None
os.makedirs(os.path.join(out, 'full'), exist_ok=True)
os.makedirs(os.path.join(out, 'graph'), exist_ok=True)
torch.set_num_threads(4)
m, st, left = load(st_path, head)

from torch.onnx._internal.torchscript_exporter import onnx_proto_utils
onnx_proto_utils._add_onnxscript_fn = lambda model_bytes, custom_opsets: model_bytes
full = os.path.join(out, 'full', 'emotion_feel.onnx' if head else 'emotion_full.onnx')
x = torch.zeros(1, 32000)
torch.onnx.export(m, (x,), full, opset_version=opset, input_names=['wav'], output_names=['adv', 'feel'] if head else ['adv'],
                  dynamic_axes={'wav': {1: 'samples'}}, dynamo=False, do_constant_folding=False)
print(f'full: {full} {os.path.getsize(full) / 1e6:.1f} MB, opset {opset}')

model = pb.parse(open(full, 'rb').read())
graph_i = [i for i, (f, w, v) in enumerate(model) if f == 7]
assert len(graph_i) == 1
graph = pb.parse(model[graph_i[0]][2])
kept_bytes = 0
pointed = 0
names_pointed = []
not_pointed = []
new_graph = []
ops = {}
for f, w, v in graph:
    if f == 1:  # node: count op types
        node = pb.parse(v)
        op = [b for (ff, ww, b) in node if ff == 4][0].decode()
        ops[op] = ops.get(op, 0) + 1
    if f != 5:
        new_graph.append((f, w, v))
        continue
    t = pb.parse(v)
    name = [b for (ff, ww, b) in t if ff == 8][0].decode()
    raw = [b for (ff, ww, b) in t if ff == 9]
    dtype = [b for (ff, ww, b) in t if ff == 2]
    if name in st and raw and dtype == [1]:
        arr, off, ln = st[name]
        if len(raw[0]) != ln or raw[0] != arr.tobytes():
            raise SystemExit(f'{name}: the exported bytes differ from model.safetensors')
        t2 = [(ff, ww, b) for (ff, ww, b) in t if ff != 9]
        for k, val in (('location', 'model.safetensors'), ('offset', str(off)), ('length', str(ln))):
            t2.append((13, 2, pb.build([(1, 2, k.encode()), (2, 2, val.encode())])))
        t2.append((14, 0, 1))  # data_location = EXTERNAL
        new_graph.append((5, 2, pb.build(t2)))
        pointed += 1
        names_pointed.append(name)
    else:
        kept_bytes += len(raw[0]) if raw else 0
        not_pointed.append((name, len(raw[0]) if raw else 0))
        new_graph.append((f, w, v))
model[graph_i[0]] = (7, 2, pb.build(new_graph))
small = os.path.join(out, 'graph', 'emotion_adv.onnx')
open(small, 'wb').write(pb.build(model))
print(f'graph: {small} {os.path.getsize(small) / 1e3:.1f} kB; {pointed} weights point into model.safetensors; '
      f'{len(not_pointed)} initializers kept inside ({kept_bytes} bytes): {not_pointed[:12]}')
missing = sorted(set(st) - set(names_pointed))
print('safetensors tensors not used by the graph:', missing)
print('ops:', dict(sorted(ops.items())))
opsets = [pb.parse(v) for (f, w, v) in model if f == 8]
print('opset_import:', [(dict((ff, b) for ff, ww, b in o)) for o in opsets])
