#!/usr/bin/env python3
"""Run `radiobench wavloop` across every impairment scenario, FEC off and on.

WHAT THIS IS FOR

M1's acceptance criterion is not "FEC makes it sound better" -- that is an
opinion. It is: over an *identical* impairment pattern, FEC-off and FEC-on
outputs differ, and the FEC-recovery counter accounts for the difference. That
is a claim about two runs being comparable, and the only way to establish it is
to check the network counters of the two runs are the same number rather than
assuming the seed made them so.

So this script does three things, in order of importance:

  1. Refuses runs that are not valid measurements at all (see VALIDITY).
  2. Verifies each FEC-off/FEC-on pair really did see the same network.
  3. Only then prints the table.

A benchmark harness that reports numbers from a run where the thread was
starved is worse than no harness, because the numbers look fine.

VALIDITY

Two flags out of the tool are treated as fatal for a cell rather than
interesting:

  pacer_resyncs   the loop was descheduled for more than two frame intervals,
                  so frames that were due were never sent. Anything measured
                  after that point describes the machine's load, not the
                  pipeline.
  held_overflow   the harness's own release queue filled and dropped datagrams.
                  That is loss this script caused, and it would be reported as
                  network loss.

Neither is a reason to panic -- they usually mean something else was running --
but they are a reason to discard the cell and say so.

USAGE

  benchmarks/run_matrix.py                       # build-opus, generated fixture
  benchmarks/run_matrix.py --in voice.wav        # a real recording
  benchmarks/run_matrix.py --repeat 3 --seed 42
  benchmarks/run_matrix.py --markdown results.md --json results.json

Outputs land in --out-dir (default benchmarks/results/raw/, which .gitignore
keeps out of the repository). It holds one WAV per cell so the A/B is
listenable, plus the raw JSON of every run.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCENARIO_DIR = REPO / "benchmarks" / "scenarios"

# The network counters that must match across a FEC-off/FEC-on pair for the
# comparison to mean anything. Deliberately not the whole `net` object: these
# are the ones decided purely by the seeded PRNG, so a difference here is a bug
# rather than scheduling noise.
PAIRED_KEYS = (
    "considered",
    "forwarded",
    "dropped",
    "dropped_independent",
    "dropped_burst",
    "duplicated",
    "reordered",
)

STAGE_ORDER = (
    ("encode", "capture -> encode"),
    ("send", "queue -> sent"),
    ("wire", "sent -> received"),
    ("network", "network hold (modelled)"),
    ("buffer", "jitter buffer"),
    ("decode", "decode"),
    ("total", "END TO END"),
)


def generate_fixture(path: Path, seconds: float) -> None:
    """A deterministic speech-like signal.

    Not a sine. Concealment extrapolates from what it last decoded, and a steady
    tone is exactly what that does well -- benchmarking against one would make
    concealment look far better than it is and shrink the FEC difference this
    matrix exists to measure. Gliding pitch, three harmonics and an amplitude
    envelope give it something to get wrong.

    A real voice recording is better for the listening half of the acceptance
    criterion; pass one with --in. This exists so the matrix runs on a fresh
    clone with no binary fixture committed to the repository.
    """
    rate = 48_000
    frames = bytearray()
    for i in range(int(rate * seconds)):
        t = i / rate
        f0 = 120.0 + 60.0 * math.sin(2 * math.pi * 1.7 * t)
        s = (
            0.55 * math.sin(2 * math.pi * f0 * t)
            + 0.25 * math.sin(2 * math.pi * 2 * f0 * t + 0.6)
            + 0.12 * math.sin(2 * math.pi * 3 * f0 * t + 1.1)
        )
        s *= 0.6 + 0.4 * math.sin(2 * math.pi * 3.1 * t)
        frames += struct.pack("<h", int(s * 12_000))

    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


def read_pcm(path: Path) -> tuple[int, ...]:
    with wave.open(str(path)) as w:
        return struct.unpack(f"<{w.getnframes()}h", w.readframes(w.getnframes()))


def compare_audio(a: Path, b: Path) -> dict:
    """How far apart two playout streams are.

    The acceptance criterion says the outputs must differ *audibly*. This cannot
    judge audibility, so it reports the two numbers a person can argue with: the
    fraction of samples that differ at all, and the mean absolute difference
    against a signal whose amplitude is 12000.
    """
    left, right = read_pcm(a), read_pcm(b)
    n = min(len(left), len(right))
    if n == 0:
        return {"samples": 0, "differing_fraction": 0.0, "mean_abs_diff": 0.0}
    differing = sum(1 for i in range(n) if left[i] != right[i])
    total = sum(abs(left[i] - right[i]) for i in range(n))
    return {
        "samples": n,
        "differing_fraction": differing / n,
        "mean_abs_diff": total / n,
    }


def run_cell(binary: Path, fixture: Path, scenario: Path | None, fec: bool,
             seed: int, target_delay_ms: int, expected_loss: int,
             wav_out: Path | None, trace_out: Path | None) -> dict:
    argv = [
        str(binary), "wavloop",
        "--in", str(fixture),
        "--seed", str(seed),
        "--target-delay", str(target_delay_ms),
        "--json",
    ]
    if scenario is not None:
        argv += ["--scenario", str(scenario)]
    if fec:
        argv += ["--fec", "--expected-loss", str(expected_loss)]
    if wav_out is not None:
        argv += ["--out", str(wav_out)]
    if trace_out is not None:
        argv += ["--trace", str(trace_out)]

    completed = subprocess.run(argv, capture_output=True, text=True)
    if completed.returncode != 0:
        return {"ok": False, "reason": f"exit {completed.returncode}",
                "stderr": completed.stderr.strip(), "argv": argv}
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        return {"ok": False, "reason": f"unparseable output: {error}",
                "stdout": completed.stdout[:400], "argv": argv}

    problems = []
    if result["pacer_resyncs"] != 0:
        problems.append(f"pacer starved ({result['pacer_resyncs']} resyncs)")
    if result["held_overflow"] != 0:
        problems.append(f"harness dropped {result['held_overflow']} datagrams")

    result["ok"] = not problems
    result["reason"] = "; ".join(problems)
    result["argv"] = argv
    return result


def us(value: float) -> str:
    return f"{value / 1000.0:.1f}"


def build_tables(cells: list[dict], pairs: list[dict], baseline: dict | None,
                 seed: int, fixture: Path, repeats: int) -> str:
    lines: list[str] = []
    ap = lines.append

    ap(f"Fixture `{fixture.name}`, seed {seed}, "
       f"{repeats} repeat{'s' if repeats != 1 else ''} per cell. "
       "All times in milliseconds.")
    ap("")

    ap("### Loss recovery by scenario")
    ap("")
    ap("| Scenario | FEC | dropped | late | from packet | FEC recovered "
       "| concealed | e2e p50 | e2e p95 |")
    ap("|---|---|---|---|---|---|---|---|---|")
    for cell in cells:
        if not cell["ok"]:
            ap(f"| {cell['scenario']} | {'on' if cell['fec'] else 'off'} "
               f"| INVALID: {cell['reason']} ||||||")
            continue
        net, play, arr = cell["net"], cell["playout"], cell["arrivals"]
        total = cell["stages_us"]["total"]
        ap(f"| {cell['scenario']} | {'on' if cell['fec'] else 'off'} "
           f"| {net['dropped']} | {arr['late']} | {play['from_packet']} "
           f"| {play['fec_recovered']} | {play['concealed']} "
           f"| {us(total['p50'])} | {us(total['p95'])} |")
    ap("")

    ap("### The A/B pairs")
    ap("")
    ap("Identical seed, identical scenario, one flag changed. The first four "
       "columns must match between the two runs or the comparison is void.")
    ap("")
    ap("| Scenario | pattern identical | concealed off -> on | recovered "
       "| samples differing | mean abs diff |")
    ap("|---|---|---|---|---|---|")
    for pair in pairs:
        if not pair["comparable"]:
            ap(f"| {pair['scenario']} | **NO** ({pair['reason']}) | | | | |")
            continue
        audio = pair.get("audio") or {}
        if audio.get("samples"):
            differing = f"{audio['differing_fraction'] * 100:.1f}%"
            magnitude = f"{audio['mean_abs_diff']:.0f}"
        else:
            differing = magnitude = "-"
        ap(f"| {pair['scenario']} | yes "
           f"| {pair['concealed_off']} -> {pair['concealed_on']} "
           f"| {pair['fec_recovered']} | {differing} | {magnitude} |")
    ap("")

    # The control, and it is not optional. Turning FEC on makes Opus spend part
    # of the bitrate on redundancy, so the primary encoding gets worse whether
    # or not anything is lost. Read the divergence column without this and you
    # credit FEC with a difference it partly caused by degrading the audio.
    controls = [p for p in pairs
                if p["comparable"] and p["fec_recovered"] == 0
                and (p.get("audio") or {}).get("samples")]
    if controls:
        floor = sum(p["audio"]["mean_abs_diff"] for p in controls) / len(controls)
        names = ", ".join(f"`{p['scenario']}`" for p in controls)
        ap(f"**Control.** On {names} nothing was lost and nothing was "
           f"recovered, yet the outputs still differ by {floor:.0f} mean "
           f"absolute sample error. That is the *cost* of FEC: the encoder "
           f"spends part of its bitrate on redundancy, so the primary "
           f"encoding is worse even when the redundancy is never needed. "
           f"Subtract it before crediting FEC with the difference on the "
           f"lossy rows.")
        ap("")

    # Lateness produces holes the network counters never see, and FEC fills
    # them exactly as it fills lost ones -- to the buffer they are the same
    # hole. Worth calling out, because "dropped 0" invites the reading that
    # there was nothing for FEC to do.
    late_only = [p for p in pairs
                 if p["comparable"] and p["fec_recovered"] > 0
                 and p.get("network_dropped") == 0]
    for p in late_only:
        ap(f"**`{p['scenario']}` lost nothing on the wire** and still had "
           f"{p['fec_recovered']} frames rebuilt by FEC: the packets arrived, "
           f"but after their playout moment. A late packet and a lost one are "
           f"the same hole to the jitter buffer, so FEC repairs both.")
        ap("")

    if baseline is not None:
        ap("### Per-stage latency budget")
        ap("")
        ap(f"From the `{baseline['scenario']}` cell, FEC "
           f"{'on' if baseline['fec'] else 'off'}, "
           f"{baseline['target_delay_ms']} ms jitter buffer target.")
        ap("")
        ap("| Stage | p50 | p95 | p99 | max |")
        ap("|---|---|---|---|---|")
        for key, label in STAGE_ORDER:
            stage = baseline["stages_us"][key]
            bold = "**" if key == "total" else ""
            ap(f"| {bold}{label}{bold} | {us(stage['p50'])} "
               f"| {us(stage['p95'])} | {us(stage['p99'])} "
               f"| {us(stage['max'])} |")
        ap("")

    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default="build-opus",
                        help="build directory holding radiobench (default: build-opus)")
    parser.add_argument("--in", dest="fixture", default=None,
                        help="input WAV, 48 kHz 16-bit; generated if omitted")
    parser.add_argument("--seconds", type=float, default=8.0,
                        help="length of the generated fixture (default: 8)")
    # Under results/raw/ because .gitignore keeps raw sweeps out of the
    # repository: curated figures are committed by hand into
    # docs/measurements.md, never dumped wholesale.
    parser.add_argument("--out-dir", default="benchmarks/results/raw")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--repeat", type=int, default=1,
                        help="runs per cell; the last one is reported")
    parser.add_argument("--target-delay", type=int, default=60,
                        help="jitter buffer target, ms (default: 60)")
    parser.add_argument("--expected-loss", type=int, default=20,
                        help="FEC loss hint for the FEC-on runs (default: 20)")
    parser.add_argument("--scenarios", nargs="*", default=None,
                        help="scenario files (default: every .conf found)")
    parser.add_argument("--markdown", default=None,
                        help="write the tables here as well as to stdout")
    parser.add_argument("--traces", action="store_true",
                        help="also write a per-frame trace CSV per cell")
    args = parser.parse_args()

    binary = REPO / args.build / "tools" / "radiobench" / "radiobench"
    if not binary.exists():
        print(f"no radiobench at {binary}\n"
              f"  cmake -S . -B {args.build} -DRADIO_WITH_OPUS=ON && "
              f"cmake --build {args.build}", file=sys.stderr)
        return 2

    out_dir = REPO / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.fixture:
        fixture = Path(args.fixture).resolve()
        if not fixture.exists():
            print(f"no such fixture: {fixture}", file=sys.stderr)
            return 2
    else:
        fixture = out_dir / "fixture.wav"
        print(f"generating {args.seconds:.0f} s fixture -> {fixture}",
              file=sys.stderr)
        generate_fixture(fixture, args.seconds)

    if args.scenarios:
        scenarios = [Path(s).resolve() for s in args.scenarios]
    else:
        scenarios = sorted(SCENARIO_DIR.glob("*.conf"))
    if not scenarios:
        print(f"no scenarios in {SCENARIO_DIR}", file=sys.stderr)
        return 2

    cells: list[dict] = []
    for scenario in scenarios:
        for fec in (False, True):
            tag = f"{scenario.stem}-fec{'on' if fec else 'off'}"
            wav_out = out_dir / f"{tag}.wav"
            trace_out = out_dir / f"{tag}.trace.csv" if args.traces else None

            # Repeats exist because latency percentiles move with whatever
            # else the machine is doing; the network counters do not move at
            # all, being seeded. The last run is the one reported, so a repeat
            # is a way of letting caches and the scheduler settle rather than a
            # way of averaging noise away.
            result = {}
            for attempt in range(args.repeat):
                suffix = f" (run {attempt + 1}/{args.repeat})" if args.repeat > 1 else ""
                print(f"  {tag}{suffix}", file=sys.stderr)
                result = run_cell(binary, fixture, scenario, fec, args.seed,
                                  args.target_delay, args.expected_loss,
                                  wav_out, trace_out)

            result["scenario"] = scenario.stem
            result["fec"] = fec
            result["wav"] = str(wav_out)
            if not result["ok"]:
                print(f"    INVALID: {result['reason']}", file=sys.stderr)
            cells.append(result)

    # ---- pair the cells up and check they saw the same network --------------
    pairs = []
    for scenario in scenarios:
        name = scenario.stem
        off = next((c for c in cells if c["scenario"] == name and not c["fec"]), None)
        on = next((c for c in cells if c["scenario"] == name and c["fec"]), None)
        if off is None or on is None:
            continue
        if not off.get("ok") or not on.get("ok"):
            pairs.append({"scenario": name, "comparable": False,
                          "reason": "a run in the pair was invalid"})
            continue

        mismatched = [k for k in PAIRED_KEYS if off["net"][k] != on["net"][k]]
        if mismatched:
            # Not a tolerance failure -- these are decided by the seeded PRNG
            # alone, so any difference means the two runs did not see the same
            # network and nothing downstream can be attributed to FEC.
            pairs.append({"scenario": name, "comparable": False,
                          "reason": "network differs in " + ", ".join(mismatched)})
            continue

        pairs.append({
            "scenario": name,
            "comparable": True,
            "network_dropped": off["net"]["dropped"],
            "late": off["arrivals"]["late"],
            "concealed_off": off["playout"]["concealed"],
            "concealed_on": on["playout"]["concealed"],
            "fec_recovered": on["playout"]["fec_recovered"],
            "audio": compare_audio(Path(off["wav"]), Path(on["wav"])),
        })

    baseline = next((c for c in cells
                     if c.get("ok") and c["scenario"] == "excellent" and not c["fec"]),
                    None)
    if baseline is None:
        baseline = next((c for c in cells if c.get("ok")), None)

    tables = build_tables(cells, pairs, baseline, args.seed, fixture, args.repeat)
    print()
    print(tables)

    raw = out_dir / "matrix.json"
    raw.write_text(json.dumps({"cells": cells, "pairs": pairs}, indent=2))
    print(f"\nraw results -> {raw}", file=sys.stderr)
    if args.markdown:
        Path(args.markdown).write_text(tables + "\n")
        print(f"tables      -> {args.markdown}", file=sys.stderr)

    invalid = [c for c in cells if not c.get("ok")]
    void = [p for p in pairs if not p["comparable"]]
    if invalid:
        print(f"\n{len(invalid)} of {len(cells)} cells were not valid "
              f"measurements", file=sys.stderr)
    if void:
        print(f"{len(void)} of {len(pairs)} A/B pairs are not comparable",
              file=sys.stderr)
    return 1 if (invalid or void) else 0


if __name__ == "__main__":
    sys.exit(main())
