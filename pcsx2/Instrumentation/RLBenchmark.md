# RL Benchmark: MBAA throughput and observation baselines

This document defines the reference procedures for measuring Melty Blood: Actress Again emulation throughput and the incremental cost/correctness of reinforcement-learning observations.

## Raw mode contract

`mode: "raw"` measures only guest VSync boundaries. It does not read configured guest-memory observation ranges and does not inject benchmark-owned controller input. The benchmark requires PCSX2 to be running in `Unlimited` limiter mode; launch without `-unlimited` and the run fails instead of producing a misleading baseline.

The configuration schema remains version 1. Current benchmark results use result schema version 3; the accepted Phase 2 raw reference files used result schema version 2. Schema 3 preserves the Phase 2 fields and adds the observation range, bytes-per-observation, and trajectory-hash fields used by `observe` mode.

`decision_interval` and `input_seed` remain required by the version-1 configuration schema for forward compatibility. Raw mode does not use them to read memory or change controller state.

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

`-nogui` hides PCSX2's main application window but does not disable GS rendering or presentation. The benchmark intentionally measures the selected renderer as part of the workload. Do not mix rendered-window, surfaceless, software/null-renderer, or otherwise materially different graphics configurations in one baseline series.

## Phase 2 raw reference configuration

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

From an otherwise idle system, run:

```text
pcsx2-qt -nogui -unlimited -statefile <canonical-savestate> -rl-benchmark <config.json> <canonical-disc-image>
```

The process must return to the shell without user interaction. A successful raw result must report:

- `mode: "raw"`
- `warmup_frames: 600`
- `measured_frames: 36000`
- `success: true`
- `limiter_mode: "unlimited"`
- `unlimited: true`
- an empty `observation_ranges` array
- `observation_count: 0`
- `observation_bytes: 0`
- `bytes_per_observation: 0`
- `trajectory_hash: null`
- `synthetic_input_updates: 0`
- positive `wall_seconds` and `emulated_fps`
- matching game/build/environment metadata across all runs in the series

The aggregate emulated FPS is calculated from the benchmark's own measured VSync count divided by monotonic wall-clock time. UI performance/FPS metrics are not used as the benchmark timer.

## Accepted HX90 raw reference series

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

## Observe mode contract

`mode: "observe"` extends the same VSync benchmark with one or more EE virtual-memory ranges. Each range is read through `DebugInterface::get(BREAKPOINT_EE).ReadBytes()`, which keeps the benchmark behind PCSX2's existing EE virtual-memory/debug abstraction. Observe mode does not expose host pointers and does not add synthetic controller input.

An observe configuration adds a non-empty `observation_ranges` array:

```json
{
  "schema_version": 1,
  "mode": "observe",
  "warmup_frames": 600,
  "frames": 36000,
  "decision_interval": 4,
  "input_seed": 0,
  "output": "mbaa-observe-run-01.json",
  "observation_ranges": [
    {"address": "0x00100000", "size": 256},
    {"address": "0x00200000", "size": 128}
  ]
}
```

The addresses above demonstrate the configuration format; they are not claimed to be canonical MBAA gameplay-state variables. Use known readable EE ranges when establishing the Phase 3 reference workload.

Addresses must be hexadecimal strings with a `0x` prefix and fit in the 32-bit EE virtual-address space. Sizes must be positive unsigned 32-bit integers, and a range may not wrap past `0xFFFFFFFF`. Malformed addresses, zero/invalid sizes, empty range lists, and address-space overflow are rejected before VM startup. A syntactically valid range which PCSX2 cannot read fails the running benchmark with the range index, address, size, measured frame, and decision index in the error message.

### Decision boundaries

Warmup VSyncs are never observed. After warmup, measured VSyncs are numbered starting at 1. An observation occurs when:

```text
measured_frame % decision_interval == 0
```

For example, `decision_interval: 4` reads on measured frames 4, 8, 12, and so on. Therefore a successful run reports:

```text
observation_count = floor(measured_frames / decision_interval)
bytes_per_observation = sum(configured range sizes)
observation_bytes = observation_count * bytes_per_observation
```

Multiple discontiguous ranges are read strictly in their JSON array order. The scratch buffer is allocated before measurement begins; observation allocation is therefore not part of the measured decision-boundary path.

### Trajectory hash

Observe mode reports `trajectory_hash_algorithm: "fnv1a64"` and a 16-digit hexadecimal `trajectory_hash`. The running FNV-1a 64-bit hash starts from the standard offset basis. For every successfully completed observation, it consumes in order:

1. the 1-based measured frame index encoded as eight little-endian bytes;
2. the 0-based decision index encoded as eight little-endian bytes;
3. the exact bytes from each configured EE range, concatenated in configuration order.

The hash state is committed only after every range for that decision was read successfully. This makes repeated fixed-savestate/no-input runs directly comparable while retaining a clear failure boundary if a later range is unreadable.

### Observe-mode result fields

Result schema version 3 adds or populates:

- `observation_ranges`
- `observation_count`
- `observation_bytes`
- `bytes_per_observation`
- `trajectory_hash_algorithm`
- `trajectory_hash`

`synthetic_input_updates` remains zero in both raw and observe modes. Observe-mode `emulated_fps` uses the same measured VSync/wall-clock timer as raw mode, so the cost of the configured memory reads and hashing is included in the reported throughput.

## Phase 3 validation procedure

Use the same disc, savestate, PCSX2 settings, renderer, limiter state, measured-frame count, and host conditions for raw and observe measurements. For an acceptance run:

1. Run a raw benchmark on the Phase 3 commit to establish the directly comparable raw throughput for that build.
2. Run the chosen observe configuration at least twice from the identical canonical savestate with no synthetic input.
3. Verify that all observe runs report the same `observation_count`, `observation_bytes`, `bytes_per_observation`, observation range list, and final `trajectory_hash`.
4. Verify the expected decision count `floor(frames / decision_interval)`.
5. Test an invalid range/address and confirm the benchmark returns `success: false` with an explicit read/configuration error.
6. Run raw mode again if needed to confirm it still reports zero observations/bytes and never populates a trajectory hash.

For a comparable raw FPS value `R` and observe FPS value `O`, report the observation throughput delta as:

```text
throughput_delta_percent = 100 * (O - R) / R
```

A negative value is the percentage throughput loss introduced by the configured observation workload. Prefer a same-build raw run over the historical Phase 2 mean when measuring the incremental cost, because unrelated code/compiler/environment changes can move the baseline.

## Repeating the raw reference series

Perform three back-to-back raw runs on an otherwise idle machine with identical disc, savestate, PCSX2 build, PCSX2 settings, and workload. Use distinct output files, for example:

```text
mbaa-raw-run-01.json
mbaa-raw-run-02.json
mbaa-raw-run-03.json
```

Before comparing throughput, verify that the following fields match between all three results: `game_serial`, `game_crc`, `disc_version`, `pcsx2_commit`, `renderer`, `internal_resolution_width`, `internal_resolution_height`, `upscale_multiplier`, `mtvu`, `synchronous_mtgs`, `vsync_queue_size`, `ee_cycle_rate`, `ee_cycle_skip`, `vu_flag_hack`, `vu1_instant`, `wait_loop`, `fast_cdvd`, `thread_pinning`, `limiter_mode`, and the host CPU/count fields.

Report all three raw FPS values rather than only the fastest result. Record mean, min/max, and spread. Do not hide run-to-run variance. If the spread is unexpectedly large, repeat only after identifying and documenting the external cause (background load, thermal throttling, power-plan change, configuration mismatch, and so on).
