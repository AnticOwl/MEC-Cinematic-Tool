# The optional emotion model (Pose window > Voice > From the voice)

`emotion_feel.onnx` is made here from public downloads; it is not in the repository or the tools' main download (about
660 MB, CC BY-NC-SA 4.0: non-commercial, and audeering's model card says "for research purpose only"). `build.ps1`
builds `EmotionHelper.exe` and puts it, `README_EmotionModel.txt` and `LICENSE.txt` in `dist\EmotionModel`; add
`emotion_feel.onnx` there. The optional download is a second zip on the release page holding that folder as
`Cinematic Tools\EmotionModel\` (with `THIRD_PARTY_NOTICES.md`, for the code compiled into the helper), unpacked into
the game folder like the main zip.

## What it is

- audeering's `wav2vec2-large-robust-12-ft-emotion-msp-dim` (arousal, dominance, valence), written out in plain torch in
  `w2v2.py` from Hugging Face's Wav2Vec2 "stable layer norm" code (no `transformers` package), its weights read from
  the model's own `model.safetensors`.
- A small head (`train_head.py`: a logistic regression on the model's pooled hidden states) that gives the chances of 8
  feelings (neutral, calm, happy, sad, angry, fearful, disgust, surprised), trained on the RAVDESS speech recordings only.
- Exported by `make_onnx.py` (torch's TorchScript exporter, opset 17, the sample count free; no `onnx` package): inputs
  `wav` (1, samples) raw 16 kHz, outputs `adv` (1, 3) and `feel` (1, 8). The input normalisation of the model's processor
  (zero mean, unit variance + 1e-7) is inside the network. Windows' in-box Windows ML runs it (it can't read weights kept
  in another file, so they are inside the .onnx).

## The downloads (each in a folder of its own)

| What | Where | Check |
|---|---|---|
| `model.safetensors`, `config.json`, `preprocessor_config.json`, `README.md`, `LICENSE` | huggingface.co/audeering/wav2vec2-large-robust-12-ft-emotion-msp-dim, revision 6eba34a2485ea31cb03600241787c3a5edab8626 | model.safetensors 661,375,508 bytes, SHA-256 efa5ac1a...fae95 (Hugging Face's ETag) |
| `Audio_Speech_Actors_01-24.zip` (RAVDESS speech) | zenodo.org/records/1188976 (CC BY-NC-SA 4.0) | 208,468,073 bytes, MD5 bc696df654c87fed845eb13823edef8a (Zenodo's) |
| `download.zip` (EmoDB) | emodb.bilderbar.info/download/download.zip | 40,567,066 bytes; used only to test, never to train |

## Steps (Python 3.10, torch 2.10, numpy; run with `python -I`, the scripts set themselves to idle priority)

1. `check_torch.py <model.safetensors> <out dir>`: one second of silence must give the model card's
   `[[0.5460754 0.6062266 0.40431657]]` (measured: within 2.3e-7).
2. `embed_corpora.py <model.safetensors> <emodb\wav> <ravdess dir> embeddings.npz`: the pooled hidden states of every
   recording (on a CUDA GPU torch's default TF32 maths was used for the shipped head; the chances differ from full
   float32's by under 0.001, measured on 8 recordings).
3. `train_head.py embeddings.npz head.npz`: L2 chosen by leave-one-actor-out log loss (0.03); prints RAVDESS
   leave-one-actor-out (0.773) and EmoDB (0.542) with the confidence bands. The shipped head.npz: SHA-256 aa9c030e428333b2...
4. `make_onnx.py <model.safetensors> <out dir> 17 head.npz`: `full\emotion_feel.onnx` (and a weightless graph for
   reference). The shipped file: 661,516,496 bytes, SHA-256 f3c506866049710a... (an export with another torch version
   needn't be byte-identical; check its answers instead).
5. With `EmotionHelper.exe` next to the new file: `measure_helper.py <helper dir> <work dir> <emodb\wav> <ravdess dir>
   embeddings.npz head.npz` runs the helper on every recording (the shipped one: EmoDB 0.542, the same top feeling as
   PyTorch in 1970 of 1975 recordings; the rest differ by where a recording starts within a sample, not by the maths).
6. `mapping_eval.py embeddings.npz head.npz`: what the face shows at several Sureness settings, with the tools' own rule.

`resample.py` is the reference for the helper's resampler (`EmotionHelper.cpp`, matched within 3e-8 at the usual rates);
`pb.py` is the minimal protobuf reader / writer `make_onnx.py` uses.
