# RL Benchmark Phase 6: episodic savestate reset cost

Phase 6 adds a `reset` workload for measuring the cost and correctness of beginning reinforcement-learning episodes from one immutable savestate. It deliberately uses PCSX2's existing `VMManager::LoadState()` path. This phase does **not** add an in-memory snapshot implementation.

`reset` is an episodic form of the Phase 4/5 `control` workload: every episode starts from the same guest state, uses the same deterministic controller stream, and observes the same configured EE ranges at decision boundaries.

## Configuration

The configuration schema remains version 1. `reset` mode requires the normal control fields plus:

- `episodes`: number of episodes to run; must be greater than zero.
- `baseline_savestate`: path to the immutable savestate reloaded before every episode.

In reset mode, `frames` means **frames per episode**. `decision_interval` determines the number of control decisions per episode:

```text
decisions_per_episode = floor(frames / decision_interval)
```

Example for the 100-reset acceptance run:

```json
{
  "schema_version": 1,
  "mode": "reset",
  "warmup_frames": 600,
  "frames": 600,
  "decision_interval": 4,
  "input_seed": 12345,
  "episodes": 100,
  "baseline_savestate": "C:/mbaa/canonical-battle.p2s",
  "control_ports": [2],
  "observation_ranges": [
    {"address": "0x00100000", "size": 256},
    {"address": "0x00200000", "size": 128}
  ],
  "output": "mbaa-reset-100.json"
}
```

The observation ranges above are the ranges used by the existing MBAA determinism workload. As in the earlier phases, they are deterministic benchmark checkpoints, not a claim that those addresses are semantically complete gameplay state.

The benchmark should still be launched with the canonical disc, PCSX2 configuration, and startup savestate identity used by the other acceptance phases:

```text
pcsx2-qt -nogui -unlimited -statefile <canonical-savestate> -rl-benchmark <reset-config.json> <canonical-disc-image>
```

`baseline_savestate` is the file measured by the reset benchmark. Supplying the same file to `-statefile` keeps initial boot conditions aligned with the other phases, but does not replace the explicit measured reset operation.

## Episode/reset lifecycle

After the ordinary benchmark warmup and environment/controller validation, reset mode performs this sequence for every episode:

1. start the reset-latency timer;
2. synchronously call `VMManager::LoadState(baseline_savestate)`;
3. stop the reset-latency timer;
4. fail immediately if the normal savestate loader reports failure;
5. reset every benchmark-controlled pad to neutral;
6. reset each controlled port's SplitMix64 state back to the configured `input_seed` derivation;
7. clear the per-episode frame and decision counters;
8. read all configured EE observation ranges immediately and compute the reset checkpoint hash;
9. require the checkpoint to match the checkpoint established by the first successful reset;
10. start the episode-emulation timer and run exactly `frames` VSyncs.

The first episode also begins with an explicit measured `LoadState()`. Therefore `episodes: 100` means exactly 100 reset attempts and, on a passing run, 100 successful verified resets. This makes the acceptance run directly measure at least 100 reset cycles rather than measuring only the 99 transitions between 100 episodes.

The RL benchmark hook runs at the end of `VMManager::Internal::VSyncOnCPUThread()`. A reset initiated there therefore returns from the VSync path immediately after the benchmark hook; no remaining pre-load VSync work is applied to the newly restored guest state.

## Reset checkpoint

Immediately after every successful savestate load and controller reset, the benchmark reads the configured observation ranges in configuration order. It hashes only those exact bytes with FNV-1a 64-bit:

```text
reset_checkpoint_hash_algorithm = fnv1a64-observation-bytes-v1
```

The first successful reset establishes the expected checkpoint. Every later reset must produce the same hash. Any unreadable range or hash mismatch fails the benchmark immediately; execution does not continue from an unverified state.

Checkpoint reads are validation work rather than episode observations. They do not increment `observation_count` or `observation_bytes`. Normal decision-boundary observations continue to use the Phase 4 trajectory hash contract.

Because the controlled pads and PRNG states are reset after every savestate reload, controller state and action-generator state from the previous episode cannot intentionally leak into the next one. Each episode receives the same deterministic action sequence for the same seed and controlled-port set.

## Timing contract

Reset latency brackets only the synchronous `VMManager::LoadState()` call. Controller neutralization, PRNG/counter reinitialization, and checkpoint verification happen after the reset timer stops.

Reset mode reports two distinct throughput views:

```text
emulated_fps = total measured episode frames / emulation_seconds
wall_throughput_fps = total measured episode frames / wall_seconds
```

`emulation_seconds` contains only episode emulation after a reset has been loaded and verified. It excludes savestate reload latency and reset-checkpoint/setup work.

`wall_seconds` begins immediately before the first measured reset and ends after the last episode. It therefore includes reset reloads, checkpoint/setup work, and episode emulation. This is the end-to-end throughput seen by a training loop using this reset strategy.

Reset latency statistics are calculated over all attempted `VMManager::LoadState()` calls. A terminal failed load is retained in the latency sample even though the benchmark then fails. Percentiles use nearest-rank selection over the sorted latency samples.

Result schema version 5 adds:

- `emulation_seconds`
- `wall_throughput_fps`
- `baseline_savestate`
- `episodes_requested`
- `episode_count`
- `frames_per_episode`
- `reset_attempt_count`
- `successful_reset_count`
- `failed_reset_count`
- `reset_total_seconds`
- `reset_mean_latency_ms`
- `reset_min_latency_ms`
- `reset_max_latency_ms`
- `reset_p50_latency_ms`
- `reset_p95_latency_ms`
- `reset_checkpoint_count`
- `reset_checkpoint_mismatch_count`
- `reset_checkpoint_hash_algorithm`
- `reset_checkpoint_hash`
- `mean_episode_emulation_seconds`
- `reset_overhead_vs_episode_percent`

The existing `measured_frames`, `decision_count`, `observation_count`, `controller_updates`, action hash, and trajectory hash are aggregate totals across all completed/partially completed episode work. On a successful run:

```text
measured_frames = episodes * frames
decision_count = episodes * floor(frames / decision_interval)
observation_count = decision_count
successful_reset_count = episodes
failed_reset_count = 0
reset_checkpoint_count = episodes
reset_checkpoint_mismatch_count = 0
```

## Comparing reset cost to episode length

The result directly reports:

```text
mean_episode_emulation_seconds = emulation_seconds / episode_count
reset_overhead_vs_episode_percent =
    reset_mean_latency_seconds / mean_episode_emulation_seconds * 100
```

Choose `frames` to represent a realistic training episode (or run several configurations for short, medium, and long episodes). The percentage then answers the Phase 6 decision directly: how large the disk/compressed-savestate reload cost is relative to useful episode emulation.

For example, if a future measured run reported a 40 ms mean reset and 400 ms mean episode emulation, reset overhead would be 10% of episode emulation time. That is only an illustration of the calculation, not an MBAA measurement.

Do not implement or infer an in-memory snapshot optimization from this benchmark code. If the measured reset overhead is material enough to justify one, open a separate issue and attach the Phase 6 result JSON as evidence.

## Acceptance procedure

Use the canonical MBAA battle savestate and the same immutable disc/config/build identity used by Phase 5.

1. Build the branch using the normal Windows/MSBuild path and the CMake path used by earlier phases.
2. Run reset mode with `episodes: 100` or greater.
3. Require `success: true`, `episode_count == episodes_requested`, `reset_attempt_count == episodes_requested`, `successful_reset_count == episodes_requested`, and `failed_reset_count == 0`.
4. Require `reset_checkpoint_count == episodes_requested` and `reset_checkpoint_mismatch_count == 0`.
5. Confirm the result contains positive total/mean/min/max/p50/p95 reset latency and both throughput metrics.
6. Confirm `measured_frames`, decision/observation counts, and controller-update counts match the configured episode geometry.
7. Repeat the run from the same immutable inputs and confirm the reset checkpoint hash, action hash, and trajectory hash are identical. Host timing statistics may vary and are not deterministic-state checks.
8. Run raw, observe, and control smoke regressions to confirm the pre-existing modes still complete normally.
9. Compare `reset_mean_latency_ms`, `mean_episode_emulation_seconds`, `reset_overhead_vs_episode_percent`, `emulated_fps`, and `wall_throughput_fps` for a representative episode length before deciding whether a separate snapshot optimization issue is warranted.

Runtime acceptance measurements belong in the pull request after running the compiled branch on the MBAA reference machine; this document intentionally does not invent performance numbers before that run.
