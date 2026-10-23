#!/usr/bin/env python3
"""Turn bench/run.sh results into the markdown tables of language-benchmark.md (stdlib only).

usage: analyze.py [RESULTS_DIR]   (default ~/RIGS-Shared/files/op1-og-native/results/language-spikes)

MIDI-to-audio latency joins the probe's send log (i,note,vel,t_send_ns) with the spike's note log
(i,on,note,vel,src,t_recv_ns,t_out_ns) by (note, vel) in order. All times are CLOCK_MONOTONIC on one host.
t_out is when the first sample of the note reaches the DAC: write time + snd_pcm_delay / rate.
"""
import csv
import glob
import json
import os
import statistics
import subprocess
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    "~/RIGS-Shared/files/op1-og-native/results/language-spikes")
LANGS = ("cpp", "rust")


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def q(values, p):
    if not values:
        return float("nan")
    v = sorted(values)
    return v[min(len(v) - 1, int(len(v) * p))]


def join_midi(probe_csv, notes_csv):
    """-> (send->DAC ms list, send->receive ms list, matched, sent)"""
    try:
        probe = list(csv.DictReader(open(probe_csv)))
        notes = [r for r in csv.DictReader(open(notes_csv))
                 if r["src"] == "1" and r["on"] == "1" and int(r["t_out_ns"]) > 0]
    except OSError:
        return [], [], 0, 0
    e2e, transport, j = [], [], 0
    for p in probe:
        while j < len(notes) and (notes[j]["note"], notes[j]["vel"]) != (p["note"], p["vel"]):
            j += 1
        if j == len(notes):
            break
        t_send = int(p["t_send_ns"])
        e2e.append((int(notes[j]["t_out_ns"]) - t_send) / 1e6)
        transport.append((int(notes[j]["t_recv_ns"]) - t_send) / 1e6)
        j += 1
    return e2e, transport, len(e2e), len(probe)


def needed(lang):
    """Direct shared-library dependencies of a spike binary (readelf NEEDED), not ldd's closure."""
    here = os.path.dirname(os.path.abspath(__file__))
    binary = os.path.join(here, "..", "cpp/build/spike-cpp" if lang == "cpp" else "rust/target/release/spike-rust")
    try:
        out = subprocess.run(["readelf", "-d", binary], capture_output=True, text=True).stdout
    except OSError:
        return "?"
    return ", ".join(line.split("[")[1].rstrip("]") for line in out.splitlines() if "(NEEDED)" in line)


def build_table():
    path = os.path.join(OUT, "build.txt")
    if not os.path.exists(path):
        return "_build: not run_\n"
    times, sizes, header = {}, {}, ""
    for line in open(path):
        f = line.split()
        if line.startswith("#"):
            header = line[1:].strip()
        elif len(f) == 3:
            times.setdefault((f[0], f[1]), []).append(float(f[2]))
        elif len(f) >= 5 and f[1] == "size":
            sizes[f[0]] = (int(f[2]), int(f[4]), f[6] if len(f) > 6 else "")
    rows = ["| | C++ | Rust |", "|---|---|---|"]
    for kind, label in (("clean", "clean build, s (median of 3)"), ("incremental", "rebuild after touching the source, s")):
        rows.append(f"| {label} | " + " | ".join(
            f"{statistics.median(times.get((l, kind), [float('nan')])):.2f}" for l in LANGS) + " |")
    rows.append("| binary, bytes | " + " | ".join(f"{sizes.get(l, (0,))[0]:,}" for l in LANGS) + " |")
    rows.append("| stripped, bytes | " + " | ".join(f"{sizes.get(l, (0, 0))[1]:,}" for l in LANGS) + " |")
    rows.append("| shared libraries (direct NEEDED) | " + " | ".join(needed(l) for l in LANGS) + " |")
    return f"_{header}_\n\n" + "\n".join(rows) + "\n"


def dsp_table():
    rows = ["| core | voice | C++ ns/voice-sample | Rust ns/voice-sample | C++ x realtime | Rust x realtime | Rust vs C++ |",
            "|---|---|---|---|---|---|---|"]
    found = False
    for core, label in (("2", "P-core (cpu 2)"), ("20", "E-core (cpu 20)")):
        for voice in ("float", "fixed"):
            ns, rt = {}, {}
            for lang in LANGS:
                runs = [load(p) for p in glob.glob(os.path.join(OUT, "dsp", f"{lang}-core{core}-*.json"))]
                runs = [r for r in runs if r]
                if runs:
                    found = True
                    ns[lang] = statistics.median(r[voice]["ns_per_voice_sample"] for r in runs)
                    rt[lang] = statistics.median(r[voice]["realtime_x"] for r in runs)
            if len(ns) == 2:
                rows.append(f"| {label} | {voice} | {ns['cpp']:.2f} | {ns['rust']:.2f} | {rt['cpp']:.0f} | "
                            f"{rt['rust']:.0f} | {(ns['rust'] / ns['cpp'] - 1) * 100:+.0f} % |")
    return ("\n".join(rows) + "\n") if found else "_dsp: not run_\n"


def sweep_table():
    rows = ["| period | fill | latency target, ms | C++ xruns per run (play/capture) | Rust xruns per run | C++ out / round trip, ms | Rust out / round trip, ms |",
            "|---|---|---|---|---|---|---|"]
    stable = {}
    any_run = False
    for P, F in ((16, 2), (16, 3), (16, 4), (32, 2), (32, 3), (32, 4), (64, 2), (64, 3), (64, 4), (128, 2)):
        cells = []
        for l in LANGS:
            runs = [load(p) for p in sorted(glob.glob(os.path.join(OUT, "sweep", f"{l}-p{P}-f{F}-r*.json")))]
            runs = [r for r in runs if r]
            if not runs:
                cells.append(("-", "-"))
                continue
            any_run = True
            ok = [r for r in runs if not r.get("failed")]
            xr = ", ".join("failed" if r.get("failed") else f"{r['xruns_play']}/{r['xruns_capture']}" for r in runs)
            if ok and len(ok) == len(runs) and all(r["xruns_play"] + r["xruns_capture"] == 0 for r in ok):
                stable.setdefault(l, []).append((F * P, P, F, ok))
            lat = (f"{statistics.mean(r['output_latency_ms'] for r in ok):.2f} / "
                   f"{statistics.mean(r['roundtrip_ms'] for r in ok):.2f}") if ok else "-"
            cells.append((xr, lat))
        if any(c[0] != "-" for c in cells):
            rows.append(f"| {P} | {F} | {F * P / 44.1:.2f} | {cells[0][0]} | {cells[1][0]} | {cells[0][1]} | {cells[1][1]} |")
    if not any_run:
        return "_sweep: not run_\n", {}
    best = {l: min(v, key=lambda t: (t[0], t[1])) for l, v in stable.items()}
    return "\n".join(rows) + "\n", best


def soak_rows(sub):
    files = sorted(glob.glob(os.path.join(OUT, sub, "*.json")))
    by = {}
    for f in files:
        r = load(f)
        if r and not r.get("failed"):
            base = f[:-5]
            e2e, tr, matched, sent = join_midi(base + "-probe.csv", base + "-notes.csv")
            by.setdefault((r["period"], r["fill_periods"]), {})[r["lang"]] = (r, e2e, tr, matched, sent)
    out = []
    for (P, F), langs in sorted(by.items()):
        if not all(l in langs for l in LANGS):
            continue
        rows = [f"**period {P}, fill {F}** ({langs['cpp'][0]['device']}, {langs['cpp'][0]['duration_s']:.0f} s)", "",
                "| | C++ | Rust |", "|---|---|---|"]

        def row(label, fn):
            rows.append(f"| {label} | " + " | ".join(fn(*langs[l]) for l in LANGS) + " |")

        row("xruns playback / capture", lambda r, *a: f"{r['xruns_play']} / {r['xruns_capture']}")
        row("realtime priority (rtkit)", lambda r, *a: "yes" if r["rt"] else "no")
        row("hardware buffer, frames", lambda r, *a: f"{r['buffer']}")
        row("output latency, ms (mean playback delay at write)", lambda r, *a: f"{r['output_latency_ms']:.3f}")
        if langs["cpp"][0]["capture"]:
            row("round trip, ms (capture period + playback delay)", lambda r, *a: f"{r['roundtrip_ms']:.3f}")
        row("MIDI note -> DAC, ms p50 / p99 / max",
            lambda r, e, *a: f"{q(e, .5):.3f} / {q(e, .99):.3f} / {max(e) if e else float('nan'):.3f}")
        row("MIDI send -> receive, ms p50 / p99", lambda r, e, t, *a: f"{q(t, .5):.3f} / {q(t, .99):.3f}")
        row("notes matched / sent", lambda r, e, t, m, s: f"{m} / {s}")
        row("DSP per period, us p50 / p99 / max",
            lambda r, *a: f"{r['dsp_us']['p50']:.2f} / {r['dsp_us']['p99']:.2f} / {r['dsp_us']['max']:.1f}")
        row("DSP load, % of period p99", lambda r, *a: f"{r['dsp_load_pct']['p99']:.2f}")
        row("process CPU, % of one core", lambda r, *a: f"{r['process_cpu_pct']:.2f}")
        row("audio thread CPU, % of one core", lambda r, *a: f"{r['audio_thread_cpu_pct']:.2f}")
        row("max RSS, MB", lambda r, *a: f"{r['max_rss_kb'] / 1024:.1f}")
        if langs["cpp"][0]["window"]:
            row("UI frame CPU, ms p50 / p99 / max",
                lambda r, *a: f"{r['frame_cpu_ms']['p50']:.3f} / {r['frame_cpu_ms']['p99']:.3f} / {r['frame_cpu_ms']['max']:.2f}")
            row("UI frame interval, ms p50 / p99", lambda r, *a: f"{r['frame_interval_ms']['p50']:.2f} / {r['frame_interval_ms']['p99']:.2f}")
            row("video driver / renderer", lambda r, *a: r["video"])
        out.append("\n".join(rows) + "\n")
    return "\n".join(out) if out else f"_{sub}: not run_\n"


def main():
    print("## Build\n")
    print(build_table())
    print("## DSP throughput (offline, best of 5 per run, median of 3 runs)\n")
    print(dsp_table())
    print("## Buffer sweep\n")
    table, best = sweep_table()
    print(table)
    for l, (lat, P, F, runs) in best.items():
        print(f"- smallest setting with 0 xruns in every run, {l}: period {P} x fill {F} = {lat / 44.1:.2f} ms target, "
              f"output {statistics.mean(r['output_latency_ms'] for r in runs):.2f} ms, "
              f"round trip {statistics.mean(r['roundtrip_ms'] for r in runs):.2f} ms")
    print("\n## Soak\n")
    print(soak_rows("soak"))
    print("## PipeWire path\n")
    print(soak_rows("pipewire"))


if __name__ == "__main__":
    main()
