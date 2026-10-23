# OP-1 OG panel on the PC keyboard

Every control of the OG panel has a PC key, including the four encoder pushes, split, the sound keys,
the volume knob and the power switch. Keys are matched by physical position (SDL scancodes), so the
map is the same on any keyboard layout. Code: `src/gui/ui_input.cpp` (`kPanel`, one table that the key
handler and the on-screen legend both use), `src/gui/ui_model.cpp` (what each key does).
`./run.sh --keyboard` shows the map under the display.

## The device

TE's OP-1 guide, chapter 2 "layout" (teenage.engineering/guides/op-1/original/layout). 17 columns,
6 rows:

```
+--------+-----+----------------+--------+--------+--------+--------+-----+
|        | VOL |                |        |        |        |        | MIC |
| SPEAKER+-----+    DISPLAY     |  BLUE  | GREEN  | WHITE  | ORANGE +-----+
|        |HLP|TMP               |        |        |        |        | COM |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+-----+
|SY|DR|TP|MX|T1|T2|T3|T4| 1| 2| 3| 4| 5| 6| 7| 8|SEQ|        row 2: modes, T1-T4, sound keys 1-8
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+---+
|LF|DP|SP|  F# G# A#   C# D#   F# G# A#   C# D#    |        row 3: lift drop split | black keys
+--+--+--+                                         |
|RC|PL|ST|  F  G  A  B  C  D  E  F  G  A  B  C  D  E|        rows 4-5: rec play stop | white keys
+--+--+--+                                         |
|< |> |SH|                                         |        row 5: left right shift
+--+--+--+-----------------------------------------+
            power switch: right side of the case
```

## The PC keyboard

```
 Esc    F1  F2  F3  F4     F5  F6  F7  F8     F9  F10 F11 F12            PgUp  End
 quit   P.B P.G P.W P.O    B-  B+  G-  G+     W-  W+  O-  O+             vol+  power
        (push)             (turn: blue, green, white, orange)            PgDn
                                                                         vol-
 `     1    2    3    4    5     6   7   8   9   0   -   =    Bksp
 help  syn  drm  tape mix  tempo F#  G#  A#  .   C#  D#  mic  com          <- upper octave black keys
 Tab   Q    W    E    R    T    Y   U   I   O   P   [    ]    \
 seq   T1   T2   T3   T4   F4   G4  A4  B4  C5  D5  E5   .    seq          <- upper octave F4..E5
 Caps  A    S    D    F    G    H   J   K   L   ;   '   Enter
 .     lift drop split .   F#   G#  A#  .   C#  D#  .   rec                <- lower octave black keys
 Shift   Z   X    C    V    B   N   M   ,   .   /   Shift
 shift   rec play stop F3   G3  A3  B3  C4  D4  E4  shift                  <- lower octave F3..E4
 Ctrl          Space = play                    Ctrl          Up = lift
 (layer)                                                     <- = left  Down = drop  -> = right
```

`.` = not used. Each black key sits on the boundary of its two white keys (the number row is offset half
a key from the Q row, the A row a quarter key from the Z row), and the upper F (T) sits above the lower
F (V).

| OG panel | PC key | notes |
|---|---|---|
| encoder push blue, green, white, orange | F1 F2 F3 F4 | new; what it does per page: below |
| encoder turn blue -/+ | F5 / F6 | Ctrl = 8 detents, auto-repeat turns on; Shift = the OP-1 shift (second page) |
| encoder turn green -/+ | F7 / F8 | |
| encoder turn white -/+ | F9 / F10 | |
| encoder turn orange -/+ | F11 / F12 | the mouse wheel still turns the last used encoder |
| help (shift: tools = clock) | `` ` `` | |
| synth, drum, tape, mixer | 1 2 3 4 | shift+mixer = signal flow (guide 14.3); shift+tape = tape options (as before) |
| tempo / metronome (shift: metronome on/off) | 5 | |
| T1 T2 T3 T4 | Q W E R | |
| sequencer | Tab, \\ | |
| mic/input (shift: mic/line) | = | |
| album/com | Backspace | press = COM, shift = album (ASSUMED, see open points) |
| lift, drop, split (shift: join) | A S D | Up/Down also lift/drop |
| record, play, stop | Z X C | Enter also record, Space also play; shift+Z overdub, Z held + note starts recording |
| left, right | Left, Right arrows | synth/drum: octave -/+ (guide 3.2); tape and the rest: a beat back/forward (shift: a bar) |
| shift | Shift (both) | |
| 24 musical keys F..E | V B N M , . / + G H J L ; (F3..E4), T Y U I O P [ + 6 7 8 0 - (F4..E5) | all 24 at once; the lowest key is F3 (53) at octave 0 |
| sound keys 1-8 | Ctrl+1..8, keypad 1-8 | the only layered OP-1 keys, see "Why" |
| volume knob | PageUp / PageDown | 16 steps, scales the audio output (not the screens' scope) |
| power switch | End | off: notes off, tape stopped, black screen, every key but End ignored |

Window and the port's extra screens (not OG keys): Ctrl+K legend, Ctrl+F fullscreen, Esc (or Ctrl+Q)
quit, Ctrl+[ / Ctrl+] browse all 61 firmware screens, Ctrl+\\ back, Ctrl+, / Ctrl+. previous/next engine,
Ctrl+A album, Ctrl+B preset browser, Ctrl+C clock, Ctrl+T tune, Ctrl+G signal flow, Ctrl+L lander.

### Why this shape

The device has about 70 controls; the PC main block has about 66 keys. A playable 24-key keyboard F..E
needs either two full rows (14 white keys: only Tab..\\ is long enough) or two stacked octaves of 7 white
keys. The stacked octaves on the right half keep the device's arrangement: mode keys and the
lift/drop/split, rec/play/stop, left/right/shift block on the left (rows in the device's order), the
keyboard bottom right, the encoders along the top. T1-T4 sit right under the mode keys (on the device they
are next to them). The F-row holds the four encoders as one group: F1-F4 push, F5-F12 turn, two keys per
encoder, as each encoder spans two key columns on the device. After that, the sound keys 1-8 are the only
group with no free row: they are Ctrl+1..8 (the digits match their labels) and the keypad digits.

## Encoder push per page

Evidence, strongest first:

1. **Firmware.** The only screen that asks for a push is `fmpopup.svg` (FM radio: an arrow nudging toward
   the blue knob, ui-bindings.md "fmpopup"). The port has no FM radio, so nothing uses it yet. Key
   handling in `Screen.cpp`/`KeyHandler.cpp` is not decoded (firmware-behaviour.md §10), so per-page
   firmware behaviour is unknown.
2. **TE guides.** OP-1 field guide, "encoder commands": tapping an encoder "usually means 'confirm',
   'return to default' or access to even more extensive features"; "turning and tapping orange controls the
   metronome" (tempo); "press T1 or tap the blue encoder to confirm your choice" (engine/preset browser).
   The OG guide says nothing about pushes.
3. **Field app.** `app/src/input/controller.ts:78`: tap = reset that parameter to its default.

So the default is **reset to default** (2, 3); where a page has a better use it is listed.

| page | blue | green | white | orange | basis |
|---|---|---|---|---|---|
| synth/drum engine (T1) | knob 1 (shift: 5) back to the engine's value | knob 2 / 6 | knob 3 / 7 | knob 4 / 8 | default (2, 3) |
| envelope (T2) | attack etc. back to the sound's value (per slot, shift = second page) | | | | default |
| play mode (shift+T2) | play mode back | portamento back | - | - | default |
| effect (T3), LFO (T4) | parameter back to the effect's/LFO's value | | | | default |
| sequencer | parameter back to the sequencer's default (kSeqDefaults) | | | | default |
| tape | tape to the start | loop off | normal speed (1000), forward | track level full | default (ASSUMED meaning of "default" per encoder) |
| mixer | track 1 volume full (shift: pan centre) | track 2 | track 3 | track 4 | default (op1.db `master`) |
| mixer EQ | low back to 16383 | mid | high | - | default (op1.db) |
| mixer master | 32767 | 32767 | 8192 | 10240 | default (op1.db `master_params`) |
| mixer master effect | parameter back to 4000 | | | | default (mixer.cpp) |
| tempo | 120.0 BPM | sync internal | sync out off | metronome on/off | default; orange from the Field guide |
| tune | play mode back | portamento back | - | - | default |
| COM | first page | | | | default |
| preset browser | load the preset under the cursors onto the slot, back to the sound | same | same | same | Field guide "tap blue to confirm"; Sir (t6 review) |
| help, input, album, clock, lander, signal flow | nothing | | | | no use known |

"Back to the engine's value" is the value the snapshot showed when that engine, effect or LFO type was
loaded (captured per side by `UiModel::Impl::capture_defaults`), since the port keeps no preset defaults.

## Other new behaviour

- **Split** (TapeOp::Split, new): splits the take under the head on the selected track at the head; shift
  joins it with the nearest take on that track (guide 8.8).
- **Sound keys 1-8.** Tape and mixer: tape tricks (guide 8.12): 1 loop in, 2 loop out (loop on when in < out),
  3 loop on/off (shift: loop the take under the head), 4 break (tape stops while held), 5 reverse;
  6 chop, 7 M1, 8 M2 do nothing yet. Synth/drum: shift = preset browser for that slot (guide 14.2); without
  shift the slot's sound loads: the preset picked for it in the browser, else factory preset n of that side
  (ASSUMED default). Slots keep their pick while the app runs (not saved to disk).
- **Preset browser** (shift + sound key; t6 review, Sir's request). The lists are op1_factory.db's: blue picks
  the folder (synth: one folder per engine, so this is where the engine is chosen; drum: drum, dbox), green the
  preset in it, six rows per page. The browser opens on what the slot plays. Browsing previews: 120 ms after
  the cursors stop, the preset under them is loaded, so the keyboard and the sequencer/arpeggiator play it.
  An encoder push keeps it on the slot and goes back to the sound; leaving any other way (mode key, T key)
  restores what played before. The names are drawn in lower case: the firmware font's upper-case letters are
  pictograms (FX, LFO, ...).
- **Record combos** (t6 review, Sir: "record plus note should trigger start of playback/record"). Record arms
  (the icon blinks while stopped); record held or armed + a note starts the tape recording with that note;
  record + play starts it too. **Shift + record arms overdub**: the new audio is summed onto the track instead
  of replacing it (TapeOp::SetRecordMode; the rec icon and the growing line are orange). Record again disarms;
  shift + record while armed switches replace/overdub. Plain record always arms replace. Stop disarms.
- **Ctrl and held keys.** A note or panel key held while Ctrl goes down is released normally (before, its
  release went to the Ctrl layer and the note hung).
- **Volume knob** (CoreOp::SetOutputVolume, new): Q1.15 gain after the mixer; full = bit-identical output.
- **Power switch**: a standby in the UI (the process keeps running).

## Scripts

`--ui-script` keeps `key=`, `down=`, `up=` (SDL key names, resolved to the US-layout position; `comma`,
`period`, `slash`, `semicolon`, `minus`, `equals`, `grave`, `apostrophe`, `backslash`, `leftbracket`,
`rightbracket` also work), `enc=`, `wait=`, `shot=`, `quit`, and adds:

- `press=E` push encoder E (0-3 or blue/green/white/orange)
- `op=NAME` an OP-1 key by panel name, independent of the PC map: synth drum tape mixer tempo help input com
  seq t1-t4 lift drop split rec play stop left right s1-s8 vol+ vol- power (`op=shift+split` holds shift)

Old scripts that used the previous map need new keys: notes (`a`..`'` from C4) are now V..[ from F3,
`z`/`x` octave are now the arrows (`op=left/right`), F1-F4 T keys are now Q W E R (`op=t1..t4`), Backspace
stop is now C (`op=stop`), 6-0/-/= mode jumps are now Ctrl+A/B/C/T/G/L and `op=com`, F12 help is
`` ` `` (`op=help`), I input is `=` (`op=input`), q/r engine is Ctrl+, / Ctrl+. ,
PageUp/PageDown tape left/right are now the arrows. `tools/screens/t9_shots.py` is updated.
