# RL Benchmark Phase 4: deterministic direct controller control

Phase 4 adds a `control` workload that combines the Phase 3 EE observation path with deterministic direct DualShock 2 state changes. The benchmark drives configured controller ports through PCSX2's in-process pad APIs; it does not require SDL, keyboard, a virtual controller, or another host input source for those ports.

## Configuration

`control` mode keeps the version-1 benchmark configuration and adds one required field:

- `control_ports`: non-empty array containing controller port `1`, port `2`, or both.

Like `observe` mode, `control` mode also requires `observation_ranges`.

Example:

```json
{
  "schema_version": 1,
  "mode": "control",
  "warmup_frames": 600,
  "frames": 36000,
  "decision_interval": 4,
  "input_seed": 12345,
  "control_ports": [1, 2],
  "observation_ranges": [
    {"address": "0x00100000", "size": 64}
  ],
  "output": "control-seed-12345.json"
}
```

Every selected port must be configured as a connected DualShock 2 before the timed interval starts. Ports that are not listed in `control_ports` are never reset or written by the benchmark.

## Decision timing

Measured frames are one-based. A control decision occurs when:

```text
measured_frame % decision_interval == 0
```

At each decision boundary the benchmark, in controller-port order:

1. advances that port's deterministic PRNG once;
2. generates the next compact PS2 digital action state;
3. resets only that controlled port;
4. applies the generated state through `Pad::SetControllerState()`;
5. hashes the port identity and generated action state;
6. reads the configured EE observation ranges and hashes those bytes after the action state.

The action remains held until that port's next decision boundary. All controlled ports are reset to neutral before timing begins and again after the final measured timestamp.

A successful run therefore has:

```text
decision_count = floor(measured_frames / decision_interval)
controller_updates[p] = decision_count for every controlled port p
controller_updates[p] = 0 for every uncontrolled port p
synthetic_input_updates = sum(controller_updates)
observation_count = decision_count
```

## Independent deterministic PRNG streams

Each P1/P2 stream uses SplitMix64. The internal 64-bit state is initialized as:

```text
P1 state = input_seed XOR 0xA0761D6478BD642F
P2 state = input_seed XOR 0xE7037ED1A0B428DB
```

One SplitMix64 value is consumed per controlled port per decision. Because each port has a separate state, enabling or disabling P2 cannot change P1's action sequence and vice versa.

SplitMix64 step:

```text
state += 0x9E3779B97F4A7C15
z = state
z = (z XOR (z >> 30)) * 0xBF58476D1CE4E5B9
z = (z XOR (z >> 27)) * 0x94D049BB133111EB
result = z XOR (z >> 31)
```

All arithmetic wraps modulo 2^64.

## `ps2-digital-mask-v1` action representation

Each generated action is a 12-bit mask stored in a `u16`:

| Bit | Control |
| ---: | --- |
| 0 | D-pad Up |
| 1 | D-pad Right |
| 2 | D-pad Down |
| 3 | D-pad Left |
| 4 | Triangle |
| 5 | Circle |
| 6 | Cross |
| 7 | Square |
| 8 | L1 |
| 9 | L2 |
| 10 | R1 |
| 11 | R2 |

The low four bits are selected from nine physically valid D-pad states:

```text
neutral
up
up+right
right
down+right
down
down+left
left
up+left
```

For a SplitMix64 result `r`:

```text
direction = direction_states[r % 9]
buttons = ((r >> 8) & 0xFF) << 4
action = direction | buttons
```

The eight button bits are independent, so the representation naturally includes face buttons, shoulder buttons, multiple simultaneous buttons, and simultaneous direction + button inputs while avoiding impossible opposite D-pad pairs.

## Hashing

Both hashes use 64-bit FNV-1a, initialized to `0xCBF29CE484222325`, with prime `0x100000001B3`.

For each control decision, `action_sequence_hash` consumes:

1. measured frame index as 8 little-endian bytes;
2. zero-based decision index as 8 little-endian bytes;
3. for each controlled port in ascending order:
   - zero-based controller index as one byte;
   - 12-bit action state as two little-endian bytes.

`trajectory_hash` begins with exactly the same frame, decision, port, and action-state bytes, then appends the configured observation bytes in range order. Thus the trajectory identity commits to both what the benchmark pressed and what it observed.

`observe` mode retains the Phase 3 trajectory-hash format and does not include any control data.

## Result fields

Result schema version 4 adds control-specific fields:

- `decision_count`
- `control_ports`
- `controller_updates.p1`
- `controller_updates.p2`
- `synthetic_input_updates`
- `control_prng_algorithm`
- `control_action_encoding`
- `action_sequence_hash_algorithm`
- `action_sequence_hash`

In `control` mode, `trajectory_hash` is also populated because observations are part of the workload.

## Acceptance procedure

Use the same savestate, build, config, and seed for repeated runs.

1. Run `[1]`, `[2]`, and `[1,2]` control configurations to verify each port can be driven independently.
2. Repeat each configuration with the same seed. `action_sequence_hash` must match; with a deterministic guest trajectory, `trajectory_hash` must match as well.
3. Change only `input_seed`; the action-sequence hash must change.
4. Confirm `controller_updates` equals `decision_count` only for controlled ports.
5. Confirm visible controller behavior changes only on configured decision boundaries and generated states include diagonals, face/shoulder buttons, and direction+button combinations.
6. Re-run `raw` and `observe` smoke tests to confirm their execution behavior remains unchanged.
