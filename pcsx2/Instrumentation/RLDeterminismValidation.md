# RL Benchmark Phase 5: end-to-end determinism validation

Phase 5 turns the Phase 4 `control` workload into a repeatability gate before reinforcement-learning training. The validation helper repeatedly starts PCSX2 from the same savestate, disc, benchmark configuration, PCSX2 build/configuration, and input seed, then compares action and trajectory hashes independently from throughput.

The helper is `tools/rl_determinism.py`. Its default target is **10 consecutive identical runs**.

## Deterministic trajectory identity

Control-mode trajectory hashing is renderer- and wall-clock-independent. The running FNV-1a 64-bit hash starts from `0xCBF29CE484222325` and uses prime `0x100000001B3`.

At every configured decision boundary the benchmark contributes, in deterministic byte order:

1. the one-based measured frame/VSync index as eight little-endian bytes;
2. the zero-based decision index as eight little-endian bytes;
3. for each benchmark-controlled port in ascending controller order:
   - zero-based controller index as one byte;
   - generated PS2 digital action state as two little-endian bytes;
4. the configured EE observation ranges, byte-for-byte and in configuration order.

`action_sequence_hash` consumes the same frame, decision, controller-index, and action-state bytes, but not observation bytes. This gives the validator two independent checkpoints:

- different `action_sequence_hash`: the generated input stream diverged;
- identical `action_sequence_hash` but different `trajectory_hash`: emulation and/or observed guest state diverged.

Renderer pixels, screenshots, monotonic wall-clock timestamps, `wall_seconds`, and `emulated_fps` are not part of either hash.

For a two-port `[1, 2]` control run, the action bytes are therefore P1 followed by P2 at every decision. If only one port is benchmark-controlled, only that selected port contributes action bytes; `control_ports` is part of the comparison metadata and must match across a validation series.

## Validator behavior

For every identical run, `tools/rl_determinism.py`:

- creates a per-run copy of the benchmark JSON and changes only its `output` path;
- launches PCSX2 with `-nogui`, `-unlimited`, the same `-statefile`, and the same disc image;
- preserves stdout, stderr, generated config, and benchmark result for that run;
- independently regenerates the expected SplitMix64 action stream from `input_seed` and verifies the reported `action_sequence_hash` against it;
- compares benchmark/emulator/host metadata against run 1;
- compares final action and trajectory hashes against run 1;
- checks that the executable, source config, savestate, and disc file size/mtime identity did not change while the series was running;
- writes `validation-summary.json` with all hashes, result paths, classifications, and throughput statistics.

The default ten-run pass condition is strict: all ten runs must report `success: true`, identical comparison metadata, the expected action-sequence hash, and one identical trajectory hash.

Throughput variance is explicitly not a determinism failure. The summary records mean/min/max FPS, max-to-min spread, sample standard deviation, and coefficient of variation so performance noise remains visible without being confused with emulation divergence.

## Basic 10-run validation

Use the same canonical MBAA disc and battle savestate used for the Phase 2-4 benchmark series:

```text
python tools/rl_determinism.py \
  --pcsx2 <path-to-pcsx2-qt> \
  --config <control-config.json> \
  --statefile <canonical-savestate> \
  --disc <canonical-disc-image>
```

On Windows PowerShell the same invocation can be written on one line, or continued with PowerShell backticks.

`--runs` defaults to `10`. The default output directory is `<config-stem>-determinism` beside the source config and must be empty before the run starts. Use `--output-dir` to select another directory.

The helper launches the canonical command shape:

```text
pcsx2-qt -nogui -unlimited -statefile <savestate> -rl-benchmark <generated-run-config.json> <disc-image>
```

Additional PCSX2 switches can be inserted with repeated `--pcsx2-arg=<argument>` options. Use the equals form when the PCSX2 argument itself begins with `-`.

## Disc identity

The validator always tracks the disc's file size and modification timestamp during the series. To bind the run to a previously recorded canonical disc digest, also pass:

```text
--disc-sha256 <expected-64-hex-digit-sha256>
```

The helper always records SHA-256 for the source benchmark config and savestate in the final summary.

## Seed sensitivity acceptance check

Add `--seed-probe` to run one extra benchmark from the same savestate with `input_seed + 1`:

```text
python tools/rl_determinism.py ... --seed-probe
```

The probe passes only when:

- its reported action hash matches the helper's independently generated hash for the changed seed;
- its `action_sequence_hash` differs from the ten-run baseline;
- its `trajectory_hash` differs from the ten-run baseline.

This directly checks that changing the input seed changes both the generated action stream and the resulting trajectory identity.

## Observation sensitivity acceptance check

Prepare a second control-mode benchmark config which is byte-for-byte equivalent in meaning except for a deliberate change to `observation_ranges` (and any `output` value). The changed range must still be readable for the entire run.

Then pass it with:

```text
python tools/rl_determinism.py ... \
  --observation-probe-config <changed-observation-config.json>
```

The helper rejects a probe config which changes any base-config field other than `observation_ranges`/`output`. The probe passes only when its action hash remains identical to the baseline while its trajectory hash changes. That isolates the trajectory-hash sensitivity to observation selection rather than to a changed action stream.

## Failure classification

The summary and console distinguish the important failure classes:

- `configuration_or_environment_mismatch`: a disc/build/emulator/host/workload metadata field differs from run 1. Compare the reported field/value pairs before treating the hashes as comparable.
- `action_stream_divergence`: the final action hash differs between nominally identical runs. This is an input-generation/control-path determinism failure, not ordinary emulator throughput variance.
- `emulation_or_observation_divergence`: the action hash matches but the trajectory hash differs. The benchmark pressed the same deterministic controls, so investigate emulation state, guest observations, host input leaking into uncontrolled ports, or configuration which is not represented by the current result metadata.
- setup/execution errors exit with status `2`; a completed series with a determinism/probe failure exits with status `1`; a passing series exits with status `0`.

Phase 4 result schema version 4 exposes final action and trajectory hashes but does not store every intermediate decision hash. Accordingly the validator reports the failing run plus expected/actual final checkpoints. If a first differing decision is required, reproduce the failure with progressively shorter `frames` values or add a temporary per-decision diagnostic; do not infer the divergence point from FPS timing.

## Reference acceptance sequence

For the Issue #5 reference machine/configuration:

1. Run the default 10-run validation with the canonical MBAA disc, savestate, control config, PCSX2 build, and settings.
2. Require all ten final `action_sequence_hash` values and all ten final `trajectory_hash` values to match.
3. Repeat with `--seed-probe` and require both hashes to change for the probe.
4. Repeat with a known-readable changed-observation config via `--observation-probe-config` and require the action hash to stay fixed while the trajectory hash changes.
5. Preserve `validation-summary.json` and the per-run result JSON files with the canonical disc/savestate SHA-256 records.

Passing this sequence establishes end-to-end repeatability of the control benchmark separately from normal host-side throughput variation.
