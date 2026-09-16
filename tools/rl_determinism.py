#!/usr/bin/env python3
"""Repeat a PCSX2 RL control benchmark and validate deterministic trajectories."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any

FNV1A64_OFFSET_BASIS = 0xCBF29CE484222325
FNV1A64_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
CONTROL_PORT_SEED_XOR = (0xA0761D6478BD642F, 0xE7037ED1A0B428DB)
DIRECTION_STATES = (
    0,
    1 << 0,
    (1 << 0) | (1 << 1),
    1 << 1,
    (1 << 2) | (1 << 1),
    1 << 2,
    (1 << 2) | (1 << 3),
    1 << 3,
    (1 << 0) | (1 << 3),
)

COMPARISON_FIELDS = (
    "schema_version",
    "mode",
    "warmup_frames",
    "measured_frames",
    "decision_interval",
    "input_seed",
    "game_serial",
    "game_crc",
    "disc_version",
    "pcsx2_build",
    "pcsx2_commit",
    "renderer",
    "internal_resolution_width",
    "internal_resolution_height",
    "upscale_multiplier",
    "mtvu",
    "synchronous_mtgs",
    "vsync_queue_size",
    "ee_cycle_rate",
    "ee_cycle_skip",
    "vu_flag_hack",
    "vu1_instant",
    "wait_loop",
    "fast_cdvd",
    "thread_pinning",
    "limiter_mode",
    "unlimited",
    "host_cpu",
    "host_logical_processors",
    "host_cores",
    "host_packages",
    "observation_ranges",
    "observation_count",
    "observation_bytes",
    "bytes_per_observation",
    "trajectory_hash_algorithm",
    "decision_count",
    "control_ports",
    "controller_updates",
    "synthetic_input_updates",
    "control_prng_algorithm",
    "control_action_encoding",
    "action_sequence_hash_algorithm",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run the same PCSX2 RL control benchmark repeatedly from one savestate and "
            "fail on action-stream, environment, or trajectory divergence."
        )
    )
    parser.add_argument("--pcsx2", required=True, type=Path, help="Path to pcsx2-qt/pcsx2-qt.exe")
    parser.add_argument("--config", required=True, type=Path, help="Control-mode benchmark JSON")
    parser.add_argument("--statefile", required=True, type=Path, help="Canonical savestate")
    parser.add_argument("--disc", required=True, type=Path, help="Canonical disc image")
    parser.add_argument("--runs", type=int, default=10, help="Number of identical runs (default: 10)")
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Directory for generated configs, run results, logs, and validation-summary.json",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=300.0,
        help="Per-run timeout in seconds; 0 disables the timeout (default: 300)",
    )
    parser.add_argument(
        "--pcsx2-arg",
        action="append",
        default=[],
        help="Additional PCSX2 argument inserted before benchmark arguments; may be repeated",
    )
    parser.add_argument(
        "--disc-sha256",
        help="Optional expected SHA-256 for the canonical disc image; checked before the first run",
    )
    parser.add_argument(
        "--seed-probe",
        action="store_true",
        help="Run one extra control benchmark with input_seed + 1 and require both action and trajectory hashes to change",
    )
    parser.add_argument(
        "--observation-probe-config",
        type=Path,
        help=(
            "Optional control config with deliberately changed observation_ranges. One extra run is made; "
            "the action hash must stay identical and the trajectory hash must change."
        ),
    )
    return parser.parse_args()


def fail(message: str) -> None:
    raise RuntimeError(message)


def resolve_file(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        fail(f"{label} does not exist or is not a file: {resolved}")
    return resolved


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stat_identity(path: Path) -> dict[str, int]:
    stat = path.stat()
    return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream)
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"Failed to read JSON '{path}': {exc}")
    if not isinstance(value, dict):
        fail(f"JSON root must be an object: {path}")
    return value


def validate_control_config(config: dict[str, Any], source: Path) -> None:
    if config.get("schema_version") != 1:
        fail(f"{source}: expected schema_version 1")
    if config.get("mode") != "control":
        fail(f"{source}: determinism validation requires mode 'control'")

    for field in ("warmup_frames", "frames", "decision_interval", "input_seed"):
        value = config.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            fail(f"{source}: field '{field}' must be a non-negative integer")
    if config["frames"] <= 0:
        fail(f"{source}: field 'frames' must be greater than zero")
    if config["decision_interval"] <= 0:
        fail(f"{source}: field 'decision_interval' must be greater than zero")

    ports = config.get("control_ports")
    if not isinstance(ports, list) or not ports:
        fail(f"{source}: field 'control_ports' must be a non-empty array")
    if any(type(port) is not int or port not in (1, 2) for port in ports) or len(set(ports)) != len(ports):
        fail(f"{source}: control_ports must contain unique controller ports 1 and/or 2")

    ranges = config.get("observation_ranges")
    if not isinstance(ranges, list) or not ranges:
        fail(f"{source}: field 'observation_ranges' must be a non-empty array")


def hash_byte(value: int, byte: int) -> int:
    return ((value ^ byte) * FNV1A64_PRIME) & MASK64


def hash_u16(value: int, number: int) -> int:
    value = hash_byte(value, number & 0xFF)
    return hash_byte(value, (number >> 8) & 0xFF)


def hash_u64(value: int, number: int) -> int:
    for shift in range(0, 64, 8):
        value = hash_byte(value, (number >> shift) & 0xFF)
    return value


def splitmix64_next(state: int) -> tuple[int, int]:
    state = (state + 0x9E3779B97F4A7C15) & MASK64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return state, (z ^ (z >> 31)) & MASK64


def expected_action_sequence_hash(config: dict[str, Any]) -> str:
    controlled = {port - 1 for port in config["control_ports"]}
    states = [config["input_seed"] ^ xor_value for xor_value in CONTROL_PORT_SEED_XOR]
    value = FNV1A64_OFFSET_BASIS
    decision_count = config["frames"] // config["decision_interval"]

    for decision_index in range(decision_count):
        measured_frame = (decision_index + 1) * config["decision_interval"]
        value = hash_u64(value, measured_frame)
        value = hash_u64(value, decision_index)
        for controller in range(2):
            if controller not in controlled:
                continue
            states[controller], random_value = splitmix64_next(states[controller])
            direction = DIRECTION_STATES[random_value % len(DIRECTION_STATES)]
            buttons = ((random_value >> 8) & 0xFF) << 4
            action_state = direction | buttons
            value = hash_byte(value, controller)
            value = hash_u16(value, action_state)

    return f"{value:016X}"


def config_for_seed(config: dict[str, Any], seed: int) -> dict[str, Any]:
    changed = copy.deepcopy(config)
    changed["input_seed"] = seed & MASK64
    return changed


def write_run_config(config: dict[str, Any], path: Path, result_path: Path) -> None:
    run_config = copy.deepcopy(config)
    run_config["output"] = str(result_path)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(run_config, stream, indent=2)
        stream.write("\n")


def build_command(
    pcsx2: Path,
    statefile: Path,
    run_config: Path,
    disc: Path,
    extra_args: list[str],
) -> list[str]:
    return [
        str(pcsx2),
        *extra_args,
        "-nogui",
        "-unlimited",
        "-statefile",
        str(statefile),
        "-rl-benchmark",
        str(run_config),
        str(disc),
    ]


def run_once(
    *,
    label: str,
    config: dict[str, Any],
    pcsx2: Path,
    statefile: Path,
    disc: Path,
    output_dir: Path,
    extra_args: list[str],
    timeout: float | None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    safe_label = label.replace(" ", "-")
    config_path = output_dir / f"{safe_label}.config.json"
    result_path = output_dir / f"{safe_label}.result.json"
    stdout_path = output_dir / f"{safe_label}.stdout.txt"
    stderr_path = output_dir / f"{safe_label}.stderr.txt"
    write_run_config(config, config_path, result_path)

    command = build_command(pcsx2, statefile, config_path, disc, extra_args)
    try:
        completed = subprocess.run(
            command,
            cwd=pcsx2.parent,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        stdout_path.write_text(exc.stdout or "", encoding="utf-8")
        stderr_path.write_text(exc.stderr or "", encoding="utf-8")
        fail(f"{label}: PCSX2 exceeded the per-run timeout")
    except OSError as exc:
        fail(f"{label}: failed to launch PCSX2: {exc}")

    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        fail(f"{label}: PCSX2 exited with code {completed.returncode}; see {stderr_path}")
    if not result_path.is_file():
        fail(f"{label}: PCSX2 did not produce benchmark result {result_path}")

    result = load_json(result_path)
    if result.get("success") is not True:
        fail(f"{label}: benchmark reported failure: {result.get('error', '')}")
    if result.get("mode") != "control":
        fail(f"{label}: benchmark result mode is not 'control'")

    run_info = {
        "label": label,
        "config": str(config_path),
        "result": str(result_path),
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "returncode": completed.returncode,
        "wall_seconds": result.get("wall_seconds"),
        "emulated_fps": result.get("emulated_fps"),
        "action_sequence_hash": result.get("action_sequence_hash"),
        "trajectory_hash": result.get("trajectory_hash"),
    }
    return result, run_info


def field_differences(expected: dict[str, Any], actual: dict[str, Any]) -> list[dict[str, Any]]:
    differences: list[dict[str, Any]] = []
    for field in COMPARISON_FIELDS:
        expected_value = expected.get(field)
        actual_value = actual.get(field)
        if actual_value != expected_value:
            differences.append({"field": field, "expected": expected_value, "actual": actual_value})
    return differences


def same_except_observation_ranges(base: dict[str, Any], probe: dict[str, Any]) -> list[str]:
    ignored = {"output", "observation_ranges"}
    keys = set(base) | set(probe)
    return sorted(key for key in keys if key not in ignored and base.get(key) != probe.get(key))


def throughput_summary(run_infos: list[dict[str, Any]]) -> dict[str, Any]:
    fps_values = [float(info["emulated_fps"]) for info in run_infos if isinstance(info.get("emulated_fps"), (int, float))]
    if not fps_values:
        return {}
    mean = statistics.fmean(fps_values)
    summary: dict[str, Any] = {
        "mean_fps": mean,
        "min_fps": min(fps_values),
        "max_fps": max(fps_values),
        "max_min_spread_percent": ((max(fps_values) - min(fps_values)) / mean * 100.0) if mean else 0.0,
    }
    if len(fps_values) > 1:
        summary["sample_standard_deviation_fps"] = statistics.stdev(fps_values)
        summary["coefficient_of_variation_percent"] = (
            summary["sample_standard_deviation_fps"] / mean * 100.0 if mean else 0.0
        )
    return summary


def ensure_inputs_unchanged(paths: dict[str, Path], identities: dict[str, dict[str, int]]) -> None:
    for label, path in paths.items():
        current = stat_identity(path)
        if current != identities[label]:
            fail(f"{label} changed while validation was running: {path}")


def main() -> int:
    args = parse_args()
    if args.runs <= 0:
        print("error: --runs must be greater than zero", file=sys.stderr)
        return 2
    if args.timeout < 0:
        print("error: --timeout cannot be negative", file=sys.stderr)
        return 2

    try:
        pcsx2 = resolve_file(args.pcsx2, "PCSX2 executable")
        config_path = resolve_file(args.config, "benchmark config")
        statefile = resolve_file(args.statefile, "savestate")
        disc = resolve_file(args.disc, "disc image")
        config = load_json(config_path)
        validate_control_config(config, config_path)

        probe_config_path: Path | None = None
        probe_config: dict[str, Any] | None = None
        if args.observation_probe_config:
            probe_config_path = resolve_file(args.observation_probe_config, "observation probe config")
            probe_config = load_json(probe_config_path)
            validate_control_config(probe_config, probe_config_path)
            differing_fields = same_except_observation_ranges(config, probe_config)
            if differing_fields:
                fail(
                    "Observation probe config must differ from the base config only in observation_ranges/output; "
                    f"different fields: {', '.join(differing_fields)}"
                )
            if probe_config.get("observation_ranges") == config.get("observation_ranges"):
                fail("Observation probe config must deliberately change observation_ranges")

        output_dir = (
            args.output_dir.expanduser().resolve()
            if args.output_dir
            else config_path.with_name(f"{config_path.stem}-determinism")
        )
        if output_dir.exists() and any(output_dir.iterdir()):
            fail(f"Output directory is not empty: {output_dir}")
        output_dir.mkdir(parents=True, exist_ok=True)

        if args.disc_sha256:
            actual_disc_sha256 = sha256_file(disc)
            if actual_disc_sha256.lower() != args.disc_sha256.lower():
                fail(
                    "Disc SHA-256 mismatch: "
                    f"expected {args.disc_sha256.lower()}, actual {actual_disc_sha256.lower()}"
                )
        else:
            actual_disc_sha256 = None

        tracked_paths = {
            "PCSX2 executable": pcsx2,
            "benchmark config": config_path,
            "savestate": statefile,
            "disc image": disc,
        }
        if probe_config_path is not None:
            tracked_paths["observation probe config"] = probe_config_path
        identities = {label: stat_identity(path) for label, path in tracked_paths.items()}

        expected_action_hash = expected_action_sequence_hash(config)
        timeout = None if args.timeout == 0 else args.timeout
        results: list[dict[str, Any]] = []
        run_infos: list[dict[str, Any]] = []

        print(f"Validating {args.runs} identical control runs")
        print(f"Expected deterministic action hash: {expected_action_hash}")
        for run_number in range(1, args.runs + 1):
            label = f"run-{run_number:02d}"
            result, info = run_once(
                label=label,
                config=config,
                pcsx2=pcsx2,
                statefile=statefile,
                disc=disc,
                output_dir=output_dir,
                extra_args=args.pcsx2_arg,
                timeout=timeout,
            )
            ensure_inputs_unchanged(tracked_paths, identities)
            if result.get("action_sequence_hash") != expected_action_hash:
                fail(
                    f"{label}: action_sequence_hash does not match the deterministic SplitMix64/FNV-1a stream: "
                    f"expected {expected_action_hash}, actual {result.get('action_sequence_hash')}"
                )
            results.append(result)
            run_infos.append(info)
            print(
                f"{label}: action={result.get('action_sequence_hash')} "
                f"trajectory={result.get('trajectory_hash')} fps={result.get('emulated_fps')}"
            )

        baseline = results[0]
        baseline_action_hash = baseline.get("action_sequence_hash")
        baseline_trajectory_hash = baseline.get("trajectory_hash")
        failures: list[dict[str, Any]] = []

        for index, result in enumerate(results[1:], start=2):
            metadata_differences = field_differences(baseline, result)
            if metadata_differences:
                failures.append(
                    {
                        "run": index,
                        "classification": "configuration_or_environment_mismatch",
                        "differences": metadata_differences,
                    }
                )
                continue
            if result.get("action_sequence_hash") != baseline_action_hash:
                failures.append(
                    {
                        "run": index,
                        "classification": "action_stream_divergence",
                        "expected_action_sequence_hash": baseline_action_hash,
                        "actual_action_sequence_hash": result.get("action_sequence_hash"),
                        "expected_trajectory_hash": baseline_trajectory_hash,
                        "actual_trajectory_hash": result.get("trajectory_hash"),
                    }
                )
                continue
            if result.get("trajectory_hash") != baseline_trajectory_hash:
                failures.append(
                    {
                        "run": index,
                        "classification": "emulation_or_observation_divergence",
                        "action_sequence_hash": baseline_action_hash,
                        "expected_trajectory_hash": baseline_trajectory_hash,
                        "actual_trajectory_hash": result.get("trajectory_hash"),
                    }
                )

        seed_probe_info: dict[str, Any] | None = None
        if args.seed_probe:
            changed_seed = (config["input_seed"] + 1) & MASK64
            changed_config = config_for_seed(config, changed_seed)
            expected_changed_action_hash = expected_action_sequence_hash(changed_config)
            changed_result, changed_info = run_once(
                label="seed-probe",
                config=changed_config,
                pcsx2=pcsx2,
                statefile=statefile,
                disc=disc,
                output_dir=output_dir,
                extra_args=args.pcsx2_arg,
                timeout=timeout,
            )
            ensure_inputs_unchanged(tracked_paths, identities)
            seed_probe_info = changed_info | {
                "input_seed": changed_seed,
                "expected_action_sequence_hash": expected_changed_action_hash,
                "action_hash_changed": changed_result.get("action_sequence_hash") != baseline_action_hash,
                "trajectory_hash_changed": changed_result.get("trajectory_hash") != baseline_trajectory_hash,
            }
            if changed_result.get("action_sequence_hash") != expected_changed_action_hash:
                failures.append(
                    {
                        "run": "seed-probe",
                        "classification": "seed_probe_action_hash_invalid",
                        "expected_action_sequence_hash": expected_changed_action_hash,
                        "actual_action_sequence_hash": changed_result.get("action_sequence_hash"),
                    }
                )
            if changed_result.get("action_sequence_hash") == baseline_action_hash:
                failures.append(
                    {
                        "run": "seed-probe",
                        "classification": "seed_probe_did_not_change_action_stream",
                        "action_sequence_hash": baseline_action_hash,
                    }
                )
            if changed_result.get("trajectory_hash") == baseline_trajectory_hash:
                failures.append(
                    {
                        "run": "seed-probe",
                        "classification": "seed_probe_did_not_change_trajectory",
                        "trajectory_hash": baseline_trajectory_hash,
                    }
                )

        observation_probe_info: dict[str, Any] | None = None
        if probe_config is not None:
            probe_result, probe_info = run_once(
                label="observation-probe",
                config=probe_config,
                pcsx2=pcsx2,
                statefile=statefile,
                disc=disc,
                output_dir=output_dir,
                extra_args=args.pcsx2_arg,
                timeout=timeout,
            )
            ensure_inputs_unchanged(tracked_paths, identities)
            observation_probe_info = probe_info | {
                "action_hash_unchanged": probe_result.get("action_sequence_hash") == baseline_action_hash,
                "trajectory_hash_changed": probe_result.get("trajectory_hash") != baseline_trajectory_hash,
            }
            if probe_result.get("action_sequence_hash") != baseline_action_hash:
                failures.append(
                    {
                        "run": "observation-probe",
                        "classification": "observation_probe_changed_action_stream",
                        "expected_action_sequence_hash": baseline_action_hash,
                        "actual_action_sequence_hash": probe_result.get("action_sequence_hash"),
                    }
                )
            if probe_result.get("trajectory_hash") == baseline_trajectory_hash:
                failures.append(
                    {
                        "run": "observation-probe",
                        "classification": "observation_probe_did_not_change_trajectory",
                        "trajectory_hash": baseline_trajectory_hash,
                    }
                )

        throughput = throughput_summary(run_infos)
        summary = {
            "schema_version": 1,
            "passed": not failures,
            "runs_requested": args.runs,
            "runs_completed": len(results),
            "input_identity": {
                "pcsx2": str(pcsx2),
                "config": str(config_path),
                "config_sha256": sha256_file(config_path),
                "statefile": str(statefile),
                "statefile_sha256": sha256_file(statefile),
                "disc": str(disc),
                "disc_sha256": actual_disc_sha256,
            },
            "expected_action_sequence_hash": expected_action_hash,
            "baseline_action_sequence_hash": baseline_action_hash,
            "baseline_trajectory_hash": baseline_trajectory_hash,
            "runs": run_infos,
            "throughput": throughput,
            "failures": failures,
            "seed_probe": seed_probe_info,
            "observation_probe": observation_probe_info,
        }
        summary_path = output_dir / "validation-summary.json"
        with summary_path.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(summary, stream, indent=2)
            stream.write("\n")

        if failures:
            print(f"Determinism validation FAILED; see {summary_path}", file=sys.stderr)
            for failure in failures:
                print(
                    f"  {failure['run']}: {failure['classification']}",
                    file=sys.stderr,
                )
            return 1

        print(
            f"Determinism validation PASSED: {args.runs}/{args.runs} runs matched "
            f"trajectory {baseline_trajectory_hash}"
        )
        if throughput:
            print(
                "Throughput variance is informational only: "
                f"mean={throughput['mean_fps']:.3f} fps, "
                f"min={throughput['min_fps']:.3f}, max={throughput['max_fps']:.3f}, "
                f"spread={throughput['max_min_spread_percent']:.3f}%"
            )
        print(f"Summary: {summary_path}")
        return 0
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
