#!/usr/bin/env python3
"""Writes the t9/t10 acceptance script: for every mode and page, a screenshot before and after turning all
four encoders (and playing a note / running the transport where it matters). Run:
  python3 tools/screens/t9_shots.py OUTDIR > /tmp/t9.script
  build/op1-og --firmware FW --device none --no-window --ui-script @/tmp/t9.script
OP-1 keys are named with op=NAME (independent of the PC key map), the port's extra screens with their Ctrl keys
(src/gui/ui_input.h). The shots contain Teenage Engineering artwork rendered from the user's own image: keep OUTDIR private."""
import sys
out = sys.argv[1] if len(sys.argv) > 1 else "shots"
s = []
def step(x): s.append(x)
def shot(name): step(f"shot={out}/t9-{name}.png")
def turn(n=6, shift=False):
    for e in ("blue", "green", "white", "orange"):
        if shift: step("down=Left Shift")
        step(f"enc={e}:{n}")
        if shift: step("up=Left Shift")
def case(name, setup, act=None, wait=150):
    for k in setup: step(k)
    step("wait=250"); shot(f"{name}-before")
    if act: act()
    else: turn()
    step(f"wait={wait}"); shot(f"{name}-after")

# synth engines in the UI's order: fm digital drwave string cluster pulse sampler phase dna dsynth voltage iter
# (ls has no screen and the factory does not build it: the UI skips it)
engines = ["fm", "t10", "drw", "st", "cls", "pls", "sampler", "pd", "id", "dsynth", "slump", "iter"]
step("op=synth")
for i, e in enumerate(engines):
    if i: step("key=ctrl+period")
    def act():
        turn(); step("down=comma"); step("down=y"); step("wait=200")
    case(f"synth-{e}", [], act, 100)
    step("up=comma"); step("up=y")
step("key=ctrl+comma"); step("wait=100"); step("key=ctrl+comma")  # back to dsynth: the engine that uses the shift page (knobs 4..7)
case("synth-dsynth-shift", [], lambda: turn(6, True))
for _ in range(3): step("key=ctrl+period"); step("wait=100")  # on to fm
case("synth-adsr", ["op=t2"], lambda: (turn(-8), step("down=comma"), step("wait=300")))
step("up=comma")
case("synth-playmode", ["down=Left Shift", "op=t2", "up=Left Shift"])
for i in range(8):
    case(f"synth-fx{i}", ["op=t3"] if i == 0 else ["op=t3"], lambda: (turn(), step("down=comma"), step("wait=200")))
    step("up=comma")
case("synth-fx-off", [], lambda: (step("down=Left Shift"), step("op=t3"), step("up=Left Shift")))
for i in range(7):
    case(f"synth-lfo{i}", ["op=t4"], lambda: (turn(), step("down=comma"), step("wait=300")))
    step("up=comma")
case("octave", [], lambda: step("op=right"), 50)
step("wait=1000")
# drum
case("drum-drum2", ["op=drum"], lambda: (turn(), step("down=comma"), step("wait=150")))
step("up=comma")
case("drum-dynaenv", ["op=t2"])
case("drum-dbox", ["op=drum", "key=ctrl+period"], lambda: (turn(), step("down=period"), step("wait=150")))
step("up=period")
# sequencers on the synth side, with the tape running
step("op=synth")
names = ["finger", "sketch", "arpeggio", "pattern", "endless", "tombola"]  # from the default (finger) on
for i, n in enumerate(names):
    setup = ["op=seq"] if i == 0 else ["down=Left Shift", "op=seq", "up=Left Shift"]
    def act():
        turn(); step("op=play"); step("down=comma"); step("wait=1200"); step("up=comma")
    case(f"seq-{n}", setup, act, 50)
    step("op=stop")
step("op=synth")
# tape: play + record a note, change track, speed
def tape_act():
    step("op=rec"); step("op=play"); step("down=comma"); step("wait=1500"); step("up=comma")
    step("op=t2"); step("enc=white:-8"); step("enc=orange:-10")
case("tape", ["op=tape"], tape_act, 50)
step("op=stop")
case("tapeconfig", ["down=Left Shift", "op=tape", "up=Left Shift"], lambda: step("op=t3"))
case("mixer", ["op=mixer"], lambda: (turn(-10), step("down=Left Shift"), step("enc=blue:8"), step("up=Left Shift"), step("op=play"), step("down=comma"), step("wait=300")))
step("up=comma"); step("op=stop")
case("mixer-eq", ["op=t2"])
case("mixer-masterfx", ["op=t3"])
case("mixer-master", ["op=t4"], lambda: (turn(), step("down=comma"), step("wait=300")))
step("up=comma")
case("tempo", ["op=tempo"], lambda: (turn(5), step("wait=500")))
case("album", ["key=ctrl+a"], lambda: (step("op=play"), step("wait=800")))
step("op=stop")
case("presetbrowser", ["key=ctrl+b"], lambda: turn(2))
case("com", ["op=com"])
case("clock", ["key=ctrl+c"], lambda: step("wait=1200"))
case("tune", ["key=ctrl+t"], lambda: turn(2))
case("signalflow", ["key=ctrl+g"], lambda: (step("op=synth"), step("down=Left Shift"), step("op=t3"), step("up=Left Shift"), step("key=ctrl+g")))
case("lander", ["key=ctrl+l"], lambda: (turn(), step("wait=800")))
case("help", ["op=synth", "op=help"])
step("op=help")
case("in", ["op=input"], lambda: (step("down=comma"), step("wait=300")))
step("up=comma"); step("op=input")
case("micline", ["down=Left Shift", "op=input", "up=Left Shift"])
step("quit")
print("\n".join(s))
