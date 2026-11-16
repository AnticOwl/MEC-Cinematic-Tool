MEC Cinematic Tools - the emotion model (an optional download)
==============================================================

What it does
------------
GAME > Pose window > Voice > "From the voice" reads the feeling in a voice file and shows it on the posed
character's face, stretch by stretch (each phrase; a long one in parts of up to 4.5 s), over the Feeling picked
there. These files do the reading:

  EmotionHelper.exe    started by the tools outside the game, at idle priority, once for each voice
  emotion_feel.onnx    the model (about 660 MB)
  cache\               made by the tools: what was read in each voice, so the same voice is read once

Where it comes from
-------------------
This folder is a separate, optional download: the second zip on the tools' GitHub release page, next to the tools'
own zip. Unpack it into the Mirror's Edge Catalyst folder like the main zip; its files go in

  <the Mirror's Edge Catalyst folder>\Cinematic Tools\EmotionModel\

Only "From the voice" uses it; the rest of the tools work without it. To remove it, delete that folder.

Instead of downloading it, you can build it from the tools' source: tools\emotion_model (its README.md says how
emotion_feel.onnx is made from the public downloads) and build.ps1, which builds EmotionHelper.exe and puts it in
dist\EmotionModel with this file, LICENSE.txt and THIRD_PARTY_NOTICES.md.

It runs on Windows' own Windows ML (nothing to install; Windows 10 version 1809 and later have it). On older Windows
the tab may say the reading failed (Windows ML there may be too old for this model).

It takes about 1.2 s to start, then about 1.5 - 1.9 s of 2 CPU threads for each 10 s it hears with the game closed
(up to about 2.9 s while the game runs; times vary with the PC). A phrase under 3 s is heard with the
voice around it, 3 s in all. It holds about 1.7 GB of memory while it reads, whatever the voice's length, and gives it
back when it ends.

How right it is
---------------
Measured on short acted studio recordings it never heard, one whole recording at a time (each actor saying the same
few sentences). How right it is on game dialogue or your own recordings isn't known.
- English actors (RAVDESS, each actor left out of the training in turn): the right one of 8 feelings for 77 % of
  the recordings; about 9 in 10 when it is 80 % sure or more.
- German actors (EmoDB, never used for training): 54 %; about 3 in 4 when it is 90 % sure or more (4 in 5 at 95 %).
Its most common slips over both: sad heard as calm, fearful as surprised, angry as happy. So a feeling shows only as
far as the model is sure of it, and the rest of the face keeps the Feeling picked; any stretch can be set by hand.

Licence: for non-commercial use only
------------------------------------
emotion_feel.onnx is licensed under CC BY-NC-SA 4.0 (Attribution-NonCommercial-ShareAlike 4.0 International; the full
text is in LICENSE.txt). It is made from:

- audeering's model "wav2vec2-large-robust-12-ft-emotion-msp-dim"
  (https://huggingface.co/audeering/wav2vec2-large-robust-12-ft-emotion-msp-dim), CC BY-NC-SA 4.0; paper:
  https://arxiv.org/abs/2203.07378. Its model card says: "Please note that this model is for research purpose only."
- the speech recordings of RAVDESS (The Ryerson Audio-Visual Database of Emotional Speech and Song), Livingstone and
  Russo (2018), https://doi.org/10.5281/zenodo.1188976, CC BY-NC-SA 4.0: a small added part that gives the chances
  of 8 feelings was trained on them.

The changes made: audeering's weights, unchanged, converted to ONNX (opset 17) with the model's own input
normalisation built in; the 8-feeling part added (a logistic regression on the model's pooled hidden states). No
recording is included. EmotionHelper.exe is part of MEC Cinematic Tools; the third-party code compiled into it
(C++/WinRT, MIT) is credited in THIRD_PARTY_NOTICES.md.
