# RL Benchmark: canonical MBAA raw-throughput baseline

This document defines the Phase 2 reference procedure for measuring maximum sustainable Melty Blood: Actress Again emulation throughput before guest-memory observations or synthetic controller input are added.

## Raw mode contract

`mode: "raw"` measures only guest VSync boundaries. It does not read configured guest-memory observation ranges and does not inject benchmark-owned controller input. The benchmark requires PCSX2 to be running in `Unlimited` limiter mode; launch without `-unlimited` and the run fails instead of producing a misleading baseline.

The configuration schema remains version 1. Raw-mode result output uses result schema version 2 and adds the active emulator/host environment fields needed to compare runs.

`decision_interval` and `input_seed` remain required by the version-1 configuration schema for forward compatibility, but raw mode does not use them to read memory or change controller state.

## Canonical workload

Use one immutable set of inputs for every run in a baseline series:

- Game: Melty Blood: Actress Again, serial `SLPM-55184`. The accepted Phase 2 reference series reports current executable CRC `E97030DF` and disc version `1.02`; do not mix results reporting a different serial, executable CRC, or disc version into the same baseline.
- Disc image: use the same image for every run. Record its SHA-256 before the first reference run. On Windows, `Get-FileHash <disc-image> -Algorithm SHA256` provides the value.
- PCSX2 build: use the same commit for all runs in a series. The result records both `pcsx2_build` and `pcsx2_commit`.
- PCSX2 configuration: use one fixed configuration/data directory for the entire series. Do not change renderer, speedhacks, game-specific overrides, internal resolution, upscale factor, or thread-pinning settings between runs.
- Savestate: use one fixed battle savestate captured after the round has begun. Record its SHA-256 and reuse that exact file.
- Battle state: the savestate itself must hold the same P1/P2 characters, Moon styles, stage, positions, health, and meter for every run. Record those values when the canonical savestate is chosen so the state can be recreated later if necessary.

Recommended baseline record:

| Item | Canonical value |
| --- | --- |
| Disc serial | `SLPM-55184` |
| Executable CRC | `E97030DF` |
| Disc version | `1.02` |
| Disc SHA-256 | record alongside canonical input archive |
| Savestate SHA-256 | record alongside canonical input archive |
| P1 character / Moon | record with canonical savestate |
| P2 character / Moon | record with canonical savestate |
| Stage | record with canonical savestate |
| Positions / health / meter | record with canonical savestate |
| PCSX2 commit | recorded by each result |

The benchmark result records the active renderer, internal resolution, upscale multiplier, MTVU, synchronous MTGS, VSync queue size, EE cycle rate/skip, selected VU/CDVD speedhacks, thread pinning, limiter mode, host CPU name, and host logical/core/package counts. Results with different environment fields should not be treated as repetitions of the same baseline.

`-nogui` hides PCSX2's main application window but does not disable GS rendering or presentation. Raw Phase 2 intentionally measures the selected renderer as part of the workload. Do not mix rendered-window, surfaceless, software/null-renderer, or otherwise materially different graphics configurations in one baseline series.

## Reference benchmark configuration

Use a 600-VSync warmup followed by 36,000 measured VSyncs:

```json
{
  "schema_version": 1,
  "mode": "raw",
  "warmup_frames": 600,
  "frames": 36000,
  "decision_interval": 1,
  "input_seed": 0,
  "output": "mbaa-raw-run-01.json"
}
```

Change only `output` between the three reference runs.

## Running the benchmark

From an otherwise idle system, run:

```text
pcsx2-qt -nogui -unlimited -statefile <canonical-savestate> -rl-benchmark <config.json> <canonical-disc-image>
```

The process must return to the shell without user interaction. A successful result must report:

- `mode: "raw"`
- `warmup_frames: 600`
- `measured_frames: 36000`
- `success: true`
- `limiter_mode: "unlimited"`
- `unlimited: true`
- `observation_count: 0`
- `observation_bytes: 0`
- `synthetic_input_updates: 0`
- positive `wall_seconds` and `emulated_fps`
- matching game/build/environment metadata across all runs in the series

The aggregate emulated FPS is calculated from the benchmark's own measured VSync count divided by monotonic wall-clock time. UI performance/FPS metrics are not used as the benchmark timer.

## Accepted HX90 reference series

The first accepted Phase 2 series was run on an AMD Ryzen 9 5900HX host using PCSX2 commit `9d9f0ca9343365a3392d04b970e8ff0d1c85a5ca`. All three results reported identical comparison metadata:

| Field | Value |
| --- | --- |
| Game serial | `SLPM-55184` |
| Executable CRC | `E97030DF` |
| Disc version | `1.02` |
| Renderer | Direct3D 12 |
| Internal resolution | 640x448 |
| Upscale multiplier | 1.0x |
| MTVU | enabled |
| Synchronous MTGS | disabled |
| VSync queue size | 2 |
| EE cycle rate / skip | 0 / 0 |
| VU flag hack | enabled |
| Instant VU1 | enabled |
| Wait-loop detection | enabled |
| Fast CDVD | disabled |
| Thread pinning | disabled |
| Limiter | unlimited |
| Host CPU | AMD Ryzen 9 5900HX |
| Host logical processors / cores / packages | 16 / 8 / 1 |
| Observation count / bytes | 0 / 0 |
| Synthetic input updates | 0 |

Each run completed the 600-frame warmup followed by exactly 36,000 measured VSyncs and returned `success: true`.

| Run | Wall seconds | Emulated FPS |
| --- | ---: | ---: |
| 1 | 37.2704623 | 965.912355 |
| 2 | 36.5304296 | 985.479788 |
| 3 | 36.8282403 | 977.510728 |
| Mean | 36.8763774 | 976.300957 |
| Min / max FPS | — | 965.912355 / 985.479788 |
| FPS range | — | 19.567433 |
| Spread `(max - min) / mean` | — | 2.004242% |
| Sample standard deviation | — | 9.839653 FPS |
| Coefficient of variation | — | 1.007850% |

The approximately 2.00% max-to-min spread and 1.01% coefficient of variation are sufficiently tight to establish a useful raw-throughput baseline on this host. Preserve all three values when comparing later observation, control, or training-loop instrumentation; do not select only the fastest run.

The JSON result schema does not contain disc-image or savestate hashes, character/Moon selections, stage, positions, health, or meter. Those canonical-input details should be stored alongside the reference input files rather than inferred from throughput results.

## Repeating the reference series

Perform three back-to-back runs on an otherwise idle machine with identical disc, savestate, PCSX2 build, PCSX2 settings, and workload. Use distinct output files, for example:

```text
mbaa-raw-run-01.json
mbaa-raw-run-02.json
mbaa-raw-run-03.json
```

Before comparing throughput, verify that the following fields match between all three results: `game_serial`, `game_crc`, `disc_version`, `pcsx2_commit`, `renderer`, `internal_resolution_width`, `internal_resolution_height`, `upscale_multiplier`, `mtvu`, `synchronous_mtgs`, `vsync_queue_size`, `ee_cycle_rate`, `ee_cycle_skip`, `vu_flag_hack`, `vu1_instant`, `wait_loop`, `fast_cdvd`, `thread_pinning`, `limiter_mode`, and the host CPU/count fields.

Report all three raw FPS values rather than only the fastest result. Record mean, min/max, and spread. Do not hide run-to-run variance. If the spread is unexpectedly large, repeat only after identifying and documenting the external cause (background load, thermal throttling, power-plan change, configuration mismatch, and so on).
