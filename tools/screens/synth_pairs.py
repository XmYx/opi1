#!/usr/bin/env python3
"""Synth screens side by side with the firmware's authored templates (t3 evidence tool).

  python3 tools/screens/synth_pairs.py script OUTDIR > s.txt        # the ui-script: every engine, every state
  build/op1-og --firmware FW --device none --no-window --ui-script @s.txt > OUTDIR/run.log
  OP1_OG_FIRMWARE=FW OP1_OG_SHEET_DIR=OUTDIR/raw build/test_gui_test_render   # authored templates, 320x160 BMP
  python3 tools/screens/synth_pairs.py compare OUTDIR                # pairs/ + summary.md

States per engine: default, each encoder +30 and -30 detents (back to the default after each), the shift page
(each encoder +30 with shift held) and a held note (C4 + E4 for 150 ms and 650 ms). compare writes
pairs/NN-<screen>-<state>.png (firmware template | port, 1x) and summary.md: for each engine the share of drawn
pixels that differ from the template at the default state, and for each state the share of pixels that changed
against the default (does the screen follow that control). The shots contain Teenage Engineering artwork rendered
from the user's own image: keep OUTDIR private."""
import os
import sys

ENGINES = ["fm", "t10", "drw", "st", "cls", "pls", "sampler", "pd", "id", "dsynth", "slump", "iter"]
NAMES = {"fm": "fm", "t10": "digital", "drw": "drwave", "st": "string", "cls": "cluster", "pls": "pulse",
         "sampler": "sampler", "pd": "phase", "id": "dna", "dsynth": "dsynth", "slump": "voltage", "iter": "iter"}
ENC = ["blue", "green", "white", "orange"]


def script(out):
    s = ["key=1", "wait=300"]

    def shot(i, e, state):
        s.append(f"shot={out}/shots/{i:02d}-{e}-{state}.png")

    for i, e in enumerate(ENGINES):
        if i:
            s += ["key=ctrl+period", "wait=300"]
        shot(i, e, "0default")
        for enc in ENC:
            s += [f"enc={enc}:30", "wait=250"]
            shot(i, e, f"{enc}+30")
            s += [f"enc={enc}:-60", "wait=250"]
            shot(i, e, f"{enc}-30")
            s += [f"enc={enc}:30", "wait=150"]
        for enc in ENC:
            s += ["down=Left Shift", f"enc={enc}:30", "wait=250"]
            shot(i, e, f"shift-{enc}+30")
            s += [f"enc={enc}:-30", "up=Left Shift", "wait=150"]
        s += ["down=comma", "down=slash", "wait=150"]
        shot(i, e, "note150ms")
        s += ["wait=500"]
        shot(i, e, "note650ms")
        s += ["up=comma", "up=slash", "wait=600"]
    s.append("quit")
    return "\n".join(s) + "\n"


def compare(out):
    from PIL import Image, ImageChops

    shots, raw = os.path.join(out, "shots"), os.path.join(out, "raw")
    pairs = os.path.join(out, "pairs")
    os.makedirs(pairs, exist_ok=True)

    def load(p):
        im = Image.open(p).convert("RGB")
        if im.size != (320, 160):
            im = im.resize((320, 160), Image.BOX)
        return im

    def drawn(im):  # mask of pixels that are not (near) black
        return im.convert("L").point(lambda v: 255 if v > 24 else 0)

    def share_diff(a, b, region):
        d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 48 else 0)
        d = ImageChops.multiply(d, region)
        n = sum(1 for v in region.get_flattened_data() if v)
        return (sum(1 for v in d.get_flattened_data() if v) / n) if n else 0.0

    rows = ["| # | screen | engine | default vs firmware template (drawn pixels that differ) | states that change the screen |",
            "|---|---|---|---|---|"]
    for i, e in enumerate(ENGINES):
        files = sorted(f for f in os.listdir(shots) if f.startswith(f"{i:02d}-{e}-"))
        if not files:
            continue
        tpl_path = os.path.join(raw, f"{e}.bmp")
        tpl = load(tpl_path) if os.path.exists(tpl_path) else Image.new("RGB", (320, 160))
        dflt = load(os.path.join(shots, f"{i:02d}-{e}-0default.png"))
        region = ImageChops.lighter(drawn(tpl), drawn(dflt))
        tdiff = share_diff(tpl, dflt, region)
        moved = []
        for f in files:
            state = f[len(f"{i:02d}-{e}-"):-4]
            im = load(os.path.join(shots, f))
            pair = Image.new("RGB", (652, 172), (40, 40, 40))
            pair.paste(tpl, (4, 8))
            pair.paste(im, (328, 8))
            pair.save(os.path.join(pairs, f))
            if state != "0default":
                ch = share_diff(dflt, im, ImageChops.lighter(drawn(dflt), drawn(im)))
                moved.append(f"{state} {ch * 100:.0f}%")
        rows.append(f"| {i:02d} | {e} | {NAMES[e]} | {tdiff * 100:.1f}% | {', '.join(moved)} |")
    text = ("# Synth screens: port vs firmware template\n\n"
            "Left = the firmware's authored template (raw SVG, as `run.sh --screen NAME` shows it), right = the port.\n"
            "Pixels count as different when a channel differs by more than 48/255; shares are of the pixels drawn in "
            "either image. A template that overlays several states (fm: nine layouts; id: all tiles stacked; drw: "
            "empty, drawn by code) cannot match one live state pixel for pixel: see the notes.\n\n" + "\n".join(rows) + "\n")
    open(os.path.join(out, "summary.md"), "w").write(text)
    print(text)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("script", "compare"):
        sys.exit(__doc__)
    if sys.argv[1] == "script":
        os.makedirs(os.path.join(sys.argv[2], "shots"), exist_ok=True)
        sys.stdout.write(script(sys.argv[2]))
    else:
        compare(sys.argv[2])
