# Benchmarks

`run_matrix.py` runs the media pipeline across every impairment scenario with
FEC off and on, checks each run was a valid measurement, verifies the A/B pairs
really are comparable, and prints the tables.

```bash
cmake -S . -B build-opus -DRADIO_WITH_OPUS=ON && cmake --build build-opus
benchmarks/run_matrix.py --seed 42 --seconds 8
```

About 100 seconds for the full sweep. **It exits non-zero if any cell was not a
valid measurement or any pair was not comparable**, so it can be run unattended
and its exit code believed.

Curated results live in [docs/measurements.md](../docs/measurements.md) §8.
Everything a run produces goes to `results/raw/` and is gitignored: one WAV per
cell so the A/B is listenable, the raw JSON, and per-frame trace CSVs with
`--traces`.

## What it refuses to report

A harness that prints numbers from a run where the thread was starved is worse
than no harness, because the numbers look fine. Two flags out of the tool are
fatal for a cell:

| Flag | Meaning |
|---|---|
| `pacer_resyncs` | the loop was descheduled past two frame intervals, so frames that were due were never sent |
| `held_overflow` | the harness's own release queue filled, so it dropped datagrams the network model had forwarded |

Neither usually means a bug — most often something else was running on the
machine — but both mean the cell is not a measurement, and it is marked INVALID
rather than averaged in.

Separately, a FEC-off/FEC-on pair is only reported if both runs saw **identical**
`considered`, `forwarded`, `dropped`, `duplicated` and `reordered` counts. Those
are decided by the seeded PRNG alone, so any difference means the two runs did
not see the same network and nothing downstream can be attributed to FEC.

## What varies between runs and what does not

| Quantity | Reproducible? |
|---|---|
| network counters (dropped, duplicated, reordered) | exactly, from the seed |
| `late`, and therefore `concealed` and `fec_recovered` | within about one frame in 400 |
| latency percentiles | to the histogram's 3.1% bucket width |

`late` depends on when the process was scheduled, not on the seed. Treat
single-frame differences as noise and the network columns as exact.

## The fixture

With no `--in`, a deterministic speech-like signal is generated — gliding pitch,
three harmonics, an amplitude envelope. Not a sine: concealment extrapolates
from what it last decoded and does that to a steady tone almost perfectly, which
would flatter concealment and shrink the very difference the matrix measures.

For the listening half of the acceptance criterion a real voice recording is
better. Pass one with `--in voice.wav` (48 kHz, 16-bit). Nothing binary is
committed to the repository, so a fresh clone can still run the sweep.

## Scenarios

`scenarios/*.conf`, shared with `tools/impair` and parsed by the same code, so a
proxy run and a wavloop run cannot drift apart over what a scenario file means.
See [scenarios/README.md](scenarios/README.md).
