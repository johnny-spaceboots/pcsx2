// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Instrumentation/RLBenchmark.h"

#include "BuildVersion.h"
#include "DebugTools/DebugInterface.h"
#include "GS/GS.h"
#include "Host.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Pad/PadDualshock2.h"
#include "VMManager.h"

#include "common/Error.h"
#include "common/FileSystem.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <io.h>
#endif

#include "cpuinfo.h"
#include "fmt/format.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace RLBenchmark
{
	namespace
	{
		constexpr unsigned int CONFIG_SCHEMA_VERSION = 1;
		constexpr unsigned int RESULT_SCHEMA_VERSION = 5;
		constexpr std::uint64_t FNV1A64_OFFSET_BASIS = 14695981039346656037ull;
		constexpr std::uint64_t FNV1A64_PRIME = 1099511628211ull;
		constexpr u32 CONTROL_PORT_COUNT = 2;
		constexpr std::array<std::uint64_t, CONTROL_PORT_COUNT> CONTROL_PORT_SEED_XOR = {
			0xA0761D6478BD642Full,
			0xE7037ED1A0B428DBull,
		};

		constexpr u16 ACTION_UP = 1u << 0;
		constexpr u16 ACTION_RIGHT = 1u << 1;
		constexpr u16 ACTION_DOWN = 1u << 2;
		constexpr u16 ACTION_LEFT = 1u << 3;
		constexpr u16 ACTION_TRIANGLE = 1u << 4;
		constexpr u16 ACTION_CIRCLE = 1u << 5;
		constexpr u16 ACTION_CROSS = 1u << 6;
		constexpr u16 ACTION_SQUARE = 1u << 7;
		constexpr u16 ACTION_L1 = 1u << 8;
		constexpr u16 ACTION_L2 = 1u << 9;
		constexpr u16 ACTION_R1 = 1u << 10;
		constexpr u16 ACTION_R2 = 1u << 11;

		constexpr std::array<u16, 9> DIRECTION_STATES = {
			0,
			ACTION_UP,
			static_cast<u16>(ACTION_UP | ACTION_RIGHT),
			ACTION_RIGHT,
			static_cast<u16>(ACTION_DOWN | ACTION_RIGHT),
			ACTION_DOWN,
			static_cast<u16>(ACTION_DOWN | ACTION_LEFT),
			ACTION_LEFT,
			static_cast<u16>(ACTION_UP | ACTION_LEFT),
		};

		struct ObservationRange
		{
			u32 address = 0;
			u32 size = 0;
		};

		struct Config
		{
			std::string mode;
			std::uint64_t warmup_frames = 0;
			std::uint64_t frames = 0;
			std::uint64_t decision_interval = 0;
			std::uint64_t input_seed = 0;
			std::string output;
			std::vector<ObservationRange> observation_ranges;
			std::uint64_t observation_bytes_per_observation = 0;
			u32 max_observation_range_size = 0;
			std::array<bool, CONTROL_PORT_COUNT> control_ports = {};
			std::uint64_t episodes = 0;
			std::string baseline_savestate;
		};

		struct EnvironmentSnapshot
		{
			std::string renderer;
			int internal_width = 0;
			int internal_height = 0;
			float upscale_multiplier = 0.0f;
			bool mtvu = false;
			bool synchronous_mtgs = false;
			int vsync_queue_size = 0;
			int ee_cycle_rate = 0;
			unsigned int ee_cycle_skip = 0;
			bool vu_flag_hack = false;
			bool vu1_instant = false;
			bool wait_loop = false;
			bool fast_cdvd = false;
			bool thread_pinning = false;
			LimiterModeType limiter_mode = LimiterModeType::Nominal;
			std::string host_cpu;
			std::uint32_t host_logical_processors = 0;
			std::uint32_t host_cores = 0;
			std::uint32_t host_packages = 0;
		};

		struct ResetLatencySummary
		{
			double total_seconds = 0.0;
			double mean_seconds = 0.0;
			double min_seconds = 0.0;
			double max_seconds = 0.0;
			double p50_seconds = 0.0;
			double p95_seconds = 0.0;
		};

		enum class Phase
		{
			Disabled,
			Warmup,
			Measuring,
			Resetting,
			Finalized,
		};

		using Clock = std::chrono::steady_clock;

		Config s_config;
		EnvironmentSnapshot s_environment;
		Phase s_phase = Phase::Disabled;
		std::uint64_t s_warmup_frames_seen = 0;
		std::uint64_t s_measured_frames = 0;
		std::uint64_t s_observation_count = 0;
		std::uint64_t s_observation_bytes = 0;
		std::uint64_t s_trajectory_hash = FNV1A64_OFFSET_BASIS;
		std::vector<u8> s_observation_buffer;
		std::uint64_t s_control_decision_count = 0;
		std::array<std::uint64_t, CONTROL_PORT_COUNT> s_controller_updates = {};
		std::array<std::uint64_t, CONTROL_PORT_COUNT> s_control_rng_state = {};
		std::uint64_t s_action_sequence_hash = FNV1A64_OFFSET_BASIS;
		std::uint64_t s_episode_frames = 0;
		std::uint64_t s_episode_decision_count = 0;
		std::uint64_t s_completed_episodes = 0;
		std::uint64_t s_successful_reset_count = 0;
		std::uint64_t s_failed_reset_count = 0;
		std::uint64_t s_reset_checkpoint_count = 0;
		std::uint64_t s_reset_checkpoint_mismatch_count = 0;
		std::optional<std::uint64_t> s_reset_checkpoint_hash;
		std::vector<double> s_reset_latencies_seconds;
		double s_emulation_seconds = 0.0;
		bool s_episode_emulation_active = false;
		Clock::time_point s_episode_emulation_start;
		Clock::time_point s_measurement_start;

#ifdef _WIN32
		bool HasValidCRTHandle(std::FILE* stream)
		{
			const int fd = _fileno(stream);
			return fd >= 0 && _get_osfhandle(fd) != -1;
		}

		void EnsureBenchmarkStandardStreams()
		{
			const bool need_stdout = !HasValidCRTHandle(stdout);
			const bool need_stderr = !HasValidCRTHandle(stderr);
			if (!need_stdout && !need_stderr)
				return;

			if (GetConsoleCP() == 0 && !::AttachConsole(ATTACH_PARENT_PROCESS))
				return;

			std::FILE* reopened_stream = nullptr;
			if (need_stdout)
				freopen_s(&reopened_stream, "CONOUT$", "w", stdout);
			if (need_stderr)
				freopen_s(&reopened_stream, "CONOUT$", "w", stderr);
		}
#else
		void EnsureBenchmarkStandardStreams()
		{
		}
#endif

		bool ReadRequiredString(
			const rapidjson::Document& document, const char* name, std::string* value, Error* error)
		{
			const auto member = document.FindMember(name);
			if (member == document.MemberEnd())
			{
				Error::SetStringFmt(error, "RL benchmark config is missing required field '{}'.", name);
				return false;
			}
			if (!member->value.IsString() || member->value.GetStringLength() == 0)
			{
				Error::SetStringFmt(error, "RL benchmark config field '{}' must be a non-empty string.", name);
				return false;
			}

			value->assign(member->value.GetString(), member->value.GetStringLength());
			return true;
		}

		bool ReadRequiredUInt64(
			const rapidjson::Document& document, const char* name, std::uint64_t* value, Error* error)
		{
			const auto member = document.FindMember(name);
			if (member == document.MemberEnd())
			{
				Error::SetStringFmt(error, "RL benchmark config is missing required field '{}'.", name);
				return false;
			}
			if (!member->value.IsUint64())
			{
				Error::SetStringFmt(error, "RL benchmark config field '{}' must be an unsigned integer.", name);
				return false;
			}

			*value = member->value.GetUint64();
			return true;
		}

		bool ParseEEAddress(const std::string_view text, u32* address)
		{
			if (text.size() <= 2 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
				return false;

			std::uint64_t parsed = 0;
			const char* const begin = text.data() + 2;
			const char* const end = text.data() + text.size();
			const auto result = std::from_chars(begin, end, parsed, 16);
			if (result.ec != std::errc{} || result.ptr != end || parsed > std::numeric_limits<u32>::max())
				return false;

			*address = static_cast<u32>(parsed);
			return true;
		}

		bool ParseObservationRanges(const rapidjson::Document& document, Config* config, Error* error)
		{
			const auto ranges_member = document.FindMember("observation_ranges");
			if (ranges_member == document.MemberEnd())
			{
				Error::SetStringFmt(error, "RL benchmark {} mode requires 'observation_ranges'.", config->mode);
				return false;
			}
			if (!ranges_member->value.IsArray() || ranges_member->value.Empty())
			{
				Error::SetString(error, "RL benchmark config field 'observation_ranges' must be a non-empty array.");
				return false;
			}

			std::uint64_t bytes_per_observation = 0;
			for (rapidjson::SizeType i = 0; i < ranges_member->value.Size(); i++)
			{
				const rapidjson::Value& range_value = ranges_member->value[i];
				if (!range_value.IsObject())
				{
					Error::SetStringFmt(error, "RL benchmark observation_ranges[{}] must be an object.", i);
					return false;
				}

				const auto address_member = range_value.FindMember("address");
				if (address_member == range_value.MemberEnd() || !address_member->value.IsString())
				{
					Error::SetStringFmt(error,
						"RL benchmark observation_ranges[{}].address must be a hexadecimal string such as '0x00100000'.", i);
					return false;
				}

				ObservationRange range;
				const std::string_view address_text(
					address_member->value.GetString(), address_member->value.GetStringLength());
				if (!ParseEEAddress(address_text, &range.address))
				{
					Error::SetStringFmt(error,
						"RL benchmark observation_ranges[{}].address '{}' is not a valid 32-bit EE virtual address.", i,
						address_text);
					return false;
				}

				const auto size_member = range_value.FindMember("size");
				if (size_member == range_value.MemberEnd() || !size_member->value.IsUint() || size_member->value.GetUint() == 0)
				{
					Error::SetStringFmt(error,
						"RL benchmark observation_ranges[{}].size must be an unsigned integer greater than zero.", i);
					return false;
				}
				range.size = size_member->value.GetUint();

				const std::uint64_t range_end = static_cast<std::uint64_t>(range.address) + range.size;
				if (range_end > static_cast<std::uint64_t>(std::numeric_limits<u32>::max()) + 1ull)
				{
					Error::SetStringFmt(error,
						"RL benchmark observation_ranges[{}] at 0x{:08X} with size {} crosses the 32-bit EE address space.",
						i, range.address, range.size);
					return false;
				}

				if (bytes_per_observation > std::numeric_limits<std::uint64_t>::max() - range.size)
				{
					Error::SetString(error, "RL benchmark observation_ranges total size overflows the result counter.");
					return false;
				}
				bytes_per_observation += range.size;
				if (range.size > config->max_observation_range_size)
					config->max_observation_range_size = range.size;
				config->observation_ranges.push_back(range);
			}

			config->observation_bytes_per_observation = bytes_per_observation;
			return true;
		}

		bool ParseControlPorts(const rapidjson::Document& document, Config* config, Error* error)
		{
			const auto ports_member = document.FindMember("control_ports");
			if (ports_member == document.MemberEnd())
			{
				Error::SetStringFmt(error, "RL benchmark {} mode requires 'control_ports'.", config->mode);
				return false;
			}
			if (!ports_member->value.IsArray() || ports_member->value.Empty())
			{
				Error::SetString(error, "RL benchmark config field 'control_ports' must be a non-empty array containing 1 and/or 2.");
				return false;
			}

			for (rapidjson::SizeType i = 0; i < ports_member->value.Size(); i++)
			{
				const rapidjson::Value& port_value = ports_member->value[i];
				if (!port_value.IsUint() || port_value.GetUint() < 1 || port_value.GetUint() > CONTROL_PORT_COUNT)
				{
					Error::SetStringFmt(error,
						"RL benchmark control_ports[{}] must be controller port 1 or 2.", i);
					return false;
				}

				const u32 controller = port_value.GetUint() - 1;
				if (config->control_ports[controller])
				{
					Error::SetStringFmt(error,
						"RL benchmark control_ports contains duplicate controller port {}.", controller + 1);
					return false;
				}
				config->control_ports[controller] = true;
			}

			return true;
		}

		void AddStringMember(rapidjson::Document& document, const char* name, std::string_view value)
		{
			auto& allocator = document.GetAllocator();
			rapidjson::Value json_value;
			json_value.SetString(value.data(), static_cast<rapidjson::SizeType>(value.size()), allocator);
			document.AddMember(rapidjson::StringRef(name), json_value, allocator);
		}

		void AddNullableStringMember(rapidjson::Document& document, const char* name, const char* value)
		{
			auto& allocator = document.GetAllocator();
			rapidjson::Value json_value;
			if (value && value[0] != '\0')
				json_value.SetString(value, allocator);
			else
				json_value.SetNull();
			document.AddMember(rapidjson::StringRef(name), json_value, allocator);
		}

		const char* GetLimiterModeName(const LimiterModeType mode)
		{
			switch (mode)
			{
				case LimiterModeType::Nominal:
					return "nominal";
				case LimiterModeType::Turbo:
					return "turbo";
				case LimiterModeType::Slomo:
					return "slomo";
				case LimiterModeType::Unlimited:
					return "unlimited";
				default:
					return "unknown";
			}
		}

		void HashByte(std::uint64_t* hash, const u8 value)
		{
			*hash ^= value;
			*hash *= FNV1A64_PRIME;
		}

		void HashUInt16(std::uint64_t* hash, const u16 value)
		{
			HashByte(hash, static_cast<u8>(value & 0xFFu));
			HashByte(hash, static_cast<u8>((value >> 8) & 0xFFu));
		}

		void HashUInt64(std::uint64_t* hash, const std::uint64_t value)
		{
			for (unsigned int i = 0; i < 8; i++)
				HashByte(hash, static_cast<u8>((value >> (i * 8)) & 0xFFu));
		}

		void HashBytes(std::uint64_t* hash, const u8* bytes, const u32 size)
		{
			for (u32 i = 0; i < size; i++)
				HashByte(hash, bytes[i]);
		}

		std::uint64_t SplitMix64Next(std::uint64_t* state)
		{
			std::uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}

		void CaptureEnvironment()
		{
			s_environment = {};

			const GSRendererType renderer = GSGetCurrentRenderer();
			const char* renderer_name = Pcsx2Config::GSOptions::GetRendererName(renderer);
			if (renderer_name)
				s_environment.renderer = renderer_name;

			GSgetInternalResolution(&s_environment.internal_width, &s_environment.internal_height);
			s_environment.upscale_multiplier = EmuConfig.GS.UpscaleMultiplier;
			s_environment.mtvu = EmuConfig.Speedhacks.vuThread;
			s_environment.synchronous_mtgs = EmuConfig.GS.SynchronousMTGS;
			s_environment.vsync_queue_size = EmuConfig.GS.VsyncQueueSize;
			s_environment.ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
			s_environment.ee_cycle_skip = EmuConfig.Speedhacks.EECycleSkip;
			s_environment.vu_flag_hack = EmuConfig.Speedhacks.vuFlagHack;
			s_environment.vu1_instant = EmuConfig.Speedhacks.vu1Instant;
			s_environment.wait_loop = EmuConfig.Speedhacks.WaitLoop;
			s_environment.fast_cdvd = EmuConfig.Speedhacks.fastCDVD;
			s_environment.thread_pinning = EmuConfig.EnableThreadPinning;
			s_environment.limiter_mode = VMManager::GetLimiterMode();

			if (cpuinfo_initialize())
			{
				s_environment.host_logical_processors = cpuinfo_get_processors_count();
				s_environment.host_cores = cpuinfo_get_cores_count();
				s_environment.host_packages = cpuinfo_get_packages_count();
				if (s_environment.host_packages > 0)
				{
					const cpuinfo_package* package = cpuinfo_get_package(0);
					if (package && package->name[0] != '\0')
						s_environment.host_cpu = package->name;
				}
			}
		}

		bool ReadObservationRanges(std::uint64_t* next_hash, const std::uint64_t measured_frame,
			const std::uint64_t decision_index, std::string_view mode, std::string* error_text)
		{
			DebugInterface& ee = DebugInterface::get(BREAKPOINT_EE);
			for (std::size_t i = 0; i < s_config.observation_ranges.size(); i++)
			{
				const ObservationRange& range = s_config.observation_ranges[i];
				if (!ee.ReadBytes(range.address, s_observation_buffer.data(), range.size))
				{
					*error_text = fmt::format(
						"RL benchmark {} mode failed to read observation range {} at EE address 0x{:08X} ({} bytes) "
						"on measured frame {} (decision {}).",
						mode, i, range.address, range.size, measured_frame, decision_index);
					return false;
				}

				s_observation_bytes += range.size;
				HashBytes(next_hash, s_observation_buffer.data(), range.size);
			}

			return true;
		}

		bool CaptureObservation(const std::uint64_t measured_frame, std::string* error_text)
		{
			const std::uint64_t decision_index = s_observation_count;
			std::uint64_t next_hash = s_trajectory_hash;
			HashUInt64(&next_hash, measured_frame);
			HashUInt64(&next_hash, decision_index);

			if (!ReadObservationRanges(&next_hash, measured_frame, decision_index, "observe", error_text))
				return false;

			s_trajectory_hash = next_hash;
			s_observation_count++;
			return true;
		}

		bool ValidateControlPorts(std::string* error_text)
		{
			for (u32 controller = 0; controller < CONTROL_PORT_COUNT; controller++)
			{
				if (!s_config.control_ports[controller])
					continue;

				if (!Pad::HasConnectedPad(static_cast<u8>(controller)))
				{
					*error_text = fmt::format(
						"RL benchmark {} mode requires P{} to be a connected DualShock 2 controller.",
						s_config.mode, controller + 1);
					return false;
				}

				PadBase* const pad = Pad::GetPad(static_cast<u8>(controller));
				if (!pad || pad->GetType() != Pad::ControllerType::DualShock2)
				{
					*error_text = fmt::format(
						"RL benchmark {} mode requires P{} to be configured as a DualShock 2 controller.",
						s_config.mode, controller + 1);
					return false;
				}
			}

			return true;
		}

		void SetActionBind(const u32 controller, const u16 action_state, const u16 bit, const u32 bind)
		{
			if ((action_state & bit) != 0)
				Pad::SetControllerState(controller, bind, 1.0f);
		}

		void ApplyControlAction(const u32 controller, const u16 action_state)
		{
			Pad::ResetControllerInputs(controller);
			SetActionBind(controller, action_state, ACTION_UP, PadDualshock2::PAD_UP);
			SetActionBind(controller, action_state, ACTION_RIGHT, PadDualshock2::PAD_RIGHT);
			SetActionBind(controller, action_state, ACTION_DOWN, PadDualshock2::PAD_DOWN);
			SetActionBind(controller, action_state, ACTION_LEFT, PadDualshock2::PAD_LEFT);
			SetActionBind(controller, action_state, ACTION_TRIANGLE, PadDualshock2::PAD_TRIANGLE);
			SetActionBind(controller, action_state, ACTION_CIRCLE, PadDualshock2::PAD_CIRCLE);
			SetActionBind(controller, action_state, ACTION_CROSS, PadDualshock2::PAD_CROSS);
			SetActionBind(controller, action_state, ACTION_SQUARE, PadDualshock2::PAD_SQUARE);
			SetActionBind(controller, action_state, ACTION_L1, PadDualshock2::PAD_L1);
			SetActionBind(controller, action_state, ACTION_L2, PadDualshock2::PAD_L2);
			SetActionBind(controller, action_state, ACTION_R1, PadDualshock2::PAD_R1);
			SetActionBind(controller, action_state, ACTION_R2, PadDualshock2::PAD_R2);
		}

		u16 GenerateControlAction(const u32 controller)
		{
			const std::uint64_t random_value = SplitMix64Next(&s_control_rng_state[controller]);
			const u16 direction = DIRECTION_STATES[random_value % DIRECTION_STATES.size()];
			const u16 buttons = static_cast<u16>(((random_value >> 8) & 0xFFu) << 4);
			return static_cast<u16>(direction | buttons);
		}

		bool CaptureControlDecision(const std::uint64_t measured_frame, const std::uint64_t decision_index,
			std::string* error_text)
		{
			std::array<u16, CONTROL_PORT_COUNT> action_states = {};

			for (u32 controller = 0; controller < CONTROL_PORT_COUNT; controller++)
			{
				if (!s_config.control_ports[controller])
					continue;

				action_states[controller] = GenerateControlAction(controller);
				ApplyControlAction(controller, action_states[controller]);
				s_controller_updates[controller]++;
			}

			std::uint64_t next_trajectory_hash = s_trajectory_hash;
			HashUInt64(&next_trajectory_hash, measured_frame);
			HashUInt64(&next_trajectory_hash, decision_index);

			std::uint64_t next_action_hash = s_action_sequence_hash;
			HashUInt64(&next_action_hash, measured_frame);
			HashUInt64(&next_action_hash, decision_index);

			for (u32 controller = 0; controller < CONTROL_PORT_COUNT; controller++)
			{
				if (!s_config.control_ports[controller])
					continue;

				HashByte(&next_trajectory_hash, static_cast<u8>(controller));
				HashUInt16(&next_trajectory_hash, action_states[controller]);
				HashByte(&next_action_hash, static_cast<u8>(controller));
				HashUInt16(&next_action_hash, action_states[controller]);
			}

			if (!ReadObservationRanges(
					&next_trajectory_hash, measured_frame, decision_index, s_config.mode, error_text))
			{
				return false;
			}

			s_trajectory_hash = next_trajectory_hash;
			s_action_sequence_hash = next_action_hash;
			s_observation_count++;
			s_control_decision_count++;
			return true;
		}

		ResetLatencySummary SummarizeResetLatencies()
		{
			ResetLatencySummary summary;
			if (s_reset_latencies_seconds.empty())
				return summary;

			std::vector<double> sorted = s_reset_latencies_seconds;
			std::sort(sorted.begin(), sorted.end());
			for (const double seconds : sorted)
				summary.total_seconds += seconds;

			summary.mean_seconds = summary.total_seconds / static_cast<double>(sorted.size());
			summary.min_seconds = sorted.front();
			summary.max_seconds = sorted.back();

			const auto nearest_rank = [&sorted](const std::size_t percentile) {
				const std::size_t rank = (sorted.size() * percentile + 99) / 100;
				return sorted[std::min(rank, sorted.size()) - 1];
			};
			summary.p50_seconds = nearest_rank(50);
			summary.p95_seconds = nearest_rank(95);
			return summary;
		}

		std::string BuildResult(bool success, std::string_view error_text, double wall_seconds)
		{
			rapidjson::Document document(rapidjson::kObjectType);
			auto& allocator = document.GetAllocator();

			document.AddMember("schema_version", RESULT_SCHEMA_VERSION, allocator);
			AddStringMember(document, "mode", s_config.mode);
			document.AddMember("warmup_frames", s_config.warmup_frames, allocator);
			document.AddMember("measured_frames", s_measured_frames, allocator);
			document.AddMember("decision_interval", s_config.decision_interval, allocator);
			document.AddMember("input_seed", s_config.input_seed, allocator);
			document.AddMember("wall_seconds", wall_seconds, allocator);

			const double emulation_seconds = s_config.mode == "reset" ? s_emulation_seconds : wall_seconds;
			document.AddMember("emulation_seconds", emulation_seconds, allocator);
			document.AddMember("emulated_fps",
			emulation_seconds > 0.0 ? static_cast<double>(s_measured_frames) / emulation_seconds : 0.0, allocator);
			document.AddMember("wall_throughput_fps",
				wall_seconds > 0.0 ? static_cast<double>(s_measured_frames) / wall_seconds : 0.0, allocator);

			const std::string game_serial = VMManager::GetDiscSerial();
			if (!game_serial.empty())
				AddStringMember(document, "game_serial", game_serial);
			else
				document.AddMember("game_serial", rapidjson::Value(rapidjson::kNullType), allocator);

			const u32 current_crc = VMManager::GetCurrentCRC();
			const u32 game_crc = current_crc != 0 ? current_crc : VMManager::GetDiscCRC();
			if (game_crc != 0)
				AddStringMember(document, "game_crc", fmt::format("{:08X}", game_crc));
			else
				document.AddMember("game_crc", rapidjson::Value(rapidjson::kNullType), allocator);

			const std::string disc_version = VMManager::GetDiscVersion();
			AddNullableStringMember(document, "disc_version", disc_version.c_str());
			AddNullableStringMember(document, "pcsx2_build", BuildVersion::GitRev);
			AddNullableStringMember(document, "pcsx2_commit", BuildVersion::GitHash);

			AddNullableStringMember(document, "renderer", s_environment.renderer.c_str());
			document.AddMember("internal_resolution_width", s_environment.internal_width, allocator);
			document.AddMember("internal_resolution_height", s_environment.internal_height, allocator);
			document.AddMember("upscale_multiplier", s_environment.upscale_multiplier, allocator);
			document.AddMember("mtvu", s_environment.mtvu, allocator);
			document.AddMember("synchronous_mtgs", s_environment.synchronous_mtgs, allocator);
			document.AddMember("vsync_queue_size", s_environment.vsync_queue_size, allocator);
			document.AddMember("ee_cycle_rate", s_environment.ee_cycle_rate, allocator);
			document.AddMember("ee_cycle_skip", s_environment.ee_cycle_skip, allocator);
			document.AddMember("vu_flag_hack", s_environment.vu_flag_hack, allocator);
			document.AddMember("vu1_instant", s_environment.vu1_instant, allocator);
			document.AddMember("wait_loop", s_environment.wait_loop, allocator);
			document.AddMember("fast_cdvd", s_environment.fast_cdvd, allocator);
			document.AddMember("thread_pinning", s_environment.thread_pinning, allocator);
			AddStringMember(document, "limiter_mode", GetLimiterModeName(s_environment.limiter_mode));
			document.AddMember("unlimited", s_environment.limiter_mode == LimiterModeType::Unlimited, allocator);

			AddNullableStringMember(document, "host_cpu", s_environment.host_cpu.c_str());
			document.AddMember("host_logical_processors", s_environment.host_logical_processors, allocator);
			document.AddMember("host_cores", s_environment.host_cores, allocator);
			document.AddMember("host_packages", s_environment.host_packages, allocator);

			rapidjson::Value observation_ranges(rapidjson::kArrayType);
			for (const ObservationRange& range : s_config.observation_ranges)
			{
				rapidjson::Value range_value(rapidjson::kObjectType);
				const std::string address = fmt::format("0x{:08X}", range.address);
				rapidjson::Value address_value;
				address_value.SetString(address.data(), static_cast<rapidjson::SizeType>(address.size()), allocator);
				range_value.AddMember("address", address_value, allocator);
				range_value.AddMember("size", range.size, allocator);
				observation_ranges.PushBack(range_value, allocator);
			}
			document.AddMember("observation_ranges", observation_ranges, allocator);
			document.AddMember("observation_count", s_observation_count, allocator);
			document.AddMember("observation_bytes", s_observation_bytes, allocator);
			document.AddMember("bytes_per_observation", s_config.observation_bytes_per_observation, allocator);

			if (s_config.mode == "observe" || s_config.mode == "control" || s_config.mode == "reset")
			{
				AddStringMember(document, "trajectory_hash_algorithm", "fnv1a64");
				AddStringMember(document, "trajectory_hash", fmt::format("{:016X}", s_trajectory_hash));
			}
			else
			{
				document.AddMember("trajectory_hash_algorithm", rapidjson::Value(rapidjson::kNullType), allocator);
				document.AddMember("trajectory_hash", rapidjson::Value(rapidjson::kNullType), allocator);
			}

			document.AddMember("decision_count", s_control_decision_count, allocator);
			rapidjson::Value control_ports(rapidjson::kArrayType);
			for (u32 controller = 0; controller < CONTROL_PORT_COUNT; controller++)
			{
				if (s_config.control_ports[controller])
					control_ports.PushBack(controller + 1, allocator);
			}
			document.AddMember("control_ports", control_ports, allocator);

			rapidjson::Value controller_updates(rapidjson::kObjectType);
			controller_updates.AddMember("p1", s_controller_updates[0], allocator);
			controller_updates.AddMember("p2", s_controller_updates[1], allocator);
			document.AddMember("controller_updates", controller_updates, allocator);
			document.AddMember("synthetic_input_updates", s_controller_updates[0] + s_controller_updates[1], allocator);

			if (s_config.mode == "control" || s_config.mode == "reset")
			{
				AddStringMember(document, "control_prng_algorithm", "splitmix64-independent-ports-v1");
				AddStringMember(document, "control_action_encoding", "ps2-digital-mask-v1");
				AddStringMember(document, "action_sequence_hash_algorithm", "fnv1a64");
				AddStringMember(document, "action_sequence_hash", fmt::format("{:016X}", s_action_sequence_hash));
			}
			else
			{
				document.AddMember("control_prng_algorithm", rapidjson::Value(rapidjson::kNullType), allocator);
				document.AddMember("control_action_encoding", rapidjson::Value(rapidjson::kNullType), allocator);
				document.AddMember("action_sequence_hash_algorithm", rapidjson::Value(rapidjson::kNullType), allocator);
				document.AddMember("action_sequence_hash", rapidjson::Value(rapidjson::kNullType), allocator);
			}

			if (s_config.mode == "reset")
			{
				const ResetLatencySummary reset_summary = SummarizeResetLatencies();
				AddStringMember(document, "baseline_savestate", s_config.baseline_savestate);
				document.AddMember("episodes_requested", s_config.episodes, allocator);
				document.AddMember("episode_count", s_completed_episodes, allocator);
				document.AddMember("frames_per_episode", s_config.frames, allocator);
				document.AddMember("reset_attempt_count", static_cast<std::uint64_t>(s_reset_latencies_seconds.size()), allocator);
				document.AddMember("successful_reset_count", s_successful_reset_count, allocator);
				document.AddMember("failed_reset_count", s_failed_reset_count, allocator);
				document.AddMember("reset_total_seconds", reset_summary.total_seconds, allocator);
				document.AddMember("reset_mean_latency_ms", reset_summary.mean_seconds * 1000.0, allocator);
				document.AddMember("reset_min_latency_ms", reset_summary.min_seconds * 1000.0, allocator);
				document.AddMember("reset_max_latency_ms", reset_summary.max_seconds * 1000.0, allocator);
				document.AddMember("reset_p50_latency_ms", reset_summary.p50_seconds * 1000.0, allocator);
				document.AddMember("reset_p95_latency_ms", reset_summary.p95_seconds * 1000.0, allocator);
				document.AddMember("reset_checkpoint_count", s_reset_checkpoint_count, allocator);
				document.AddMember("reset_checkpoint_mismatch_count", s_reset_checkpoint_mismatch_count, allocator);
			AddStringMember(document, "reset_checkpoint_hash_algorithm", "fnv1a64-observation-bytes-v1");
				if (s_reset_checkpoint_hash.has_value())
					AddStringMember(document, "reset_checkpoint_hash", fmt::format("{:016X}", *s_reset_checkpoint_hash));
				else
					document.AddMember("reset_checkpoint_hash", rapidjson::Value(rapidjson::kNullType), allocator);

				const double mean_episode_emulation_seconds = s_completed_episodes > 0 ?
					s_emulation_seconds / static_cast<double>(s_completed_episodes) : 0.0;
				document.AddMember("mean_episode_emulation_seconds", mean_episode_emulation_seconds, allocator);
				document.AddMember("reset_overhead_vs_episode_percent",
					mean_episode_emulation_seconds > 0.0 ?
						(reset_summary.mean_seconds / mean_episode_emulation_seconds) * 100.0 : 0.0,
					allocator);
			}

			document.AddMember("success", success, allocator);
			AddStringMember(document, "error", error_text);

			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			document.Accept(writer);
			return std::string(buffer.GetString(), buffer.GetSize());
		}

		void ResetControlledPorts()
		{
			for (u32 controller = 0; controller < CONTROL_PORT_COUNT; controller++)
			{
				if (s_config.control_ports[controller])
					Pad::ResetControllerInputs(controller);
			}
		}

		void ResetControlGeneratorState()
		{
			for (u32 controller = 0; controller < CONTROL_PORT_COUNT; controller++)
				s_control_rng_state[controller] = s_config.input_seed ^ CONTROL_PORT_SEED_XOR[controller];
		}

		bool CaptureResetCheckpoint(std::uint64_t* checkpoint_hash, std::string* error_text)
		{
			std::uint64_t next_hash = FNV1A64_OFFSET_BASIS;
			DebugInterface& ee = DebugInterface::get(BREAKPOINT_EE);
			for (std::size_t i = 0; i < s_config.observation_ranges.size(); i++)
			{
				const ObservationRange& range = s_config.observation_ranges[i];
				if (!ee.ReadBytes(range.address, s_observation_buffer.data(), range.size))
				{
					*error_text = fmt::format(
						"RL benchmark reset mode failed to read reset checkpoint range {} at EE address 0x{:08X} "
						"({} bytes) before episode {}.",
						i, range.address, range.size, s_completed_episodes + 1);
					return false;
				}
				HashBytes(&next_hash, s_observation_buffer.data(), range.size);
			}

			*checkpoint_hash = next_hash;
			return true;
		}

		bool BeginResetEpisode(std::string* error_text)
		{
			s_phase = Phase::Resetting;
			const Clock::time_point reset_start = Clock::now();
			Error load_error;
			const bool load_succeeded = VMManager::LoadState(s_config.baseline_savestate.c_str(), &load_error);
			const Clock::time_point reset_end = Clock::now();
			s_reset_latencies_seconds.push_back(std::chrono::duration<double>(reset_end - reset_start).count());

			if (!load_succeeded)
			{
				s_failed_reset_count++;
				*error_text = fmt::format("RL benchmark reset mode failed to load baseline savestate '{}' before episode {}: {}",
					s_config.baseline_savestate, s_completed_episodes + 1,
					load_error.IsValid() ? load_error.GetDescription() : std::string("unknown savestate load error"));
				return false;
			}

			ResetControlledPorts();
			ResetControlGeneratorState();
			s_episode_frames = 0;
			s_episode_decision_count = 0;

			std::uint64_t checkpoint_hash = 0;
			std::string checkpoint_error;
			if (!CaptureResetCheckpoint(&checkpoint_hash, &checkpoint_error))
			{
				s_failed_reset_count++;
				*error_text = std::move(checkpoint_error);
				return false;
			}

			s_reset_checkpoint_count++;
			if (!s_reset_checkpoint_hash.has_value())
			{
				s_reset_checkpoint_hash = checkpoint_hash;
			}
			else if (*s_reset_checkpoint_hash != checkpoint_hash)
			{
				s_failed_reset_count++;
				s_reset_checkpoint_mismatch_count++;
				*error_text = fmt::format(
					"RL benchmark reset mode detected guest-state drift before episode {}: expected reset checkpoint "
					"{:016X}, observed {:016X}.",
					s_completed_episodes + 1, *s_reset_checkpoint_hash, checkpoint_hash);
				return false;
			}

			if (VMManager::GetLimiterMode() != LimiterModeType::Unlimited)
			{
				s_failed_reset_count++;
				*error_text = fmt::format(
					"RL benchmark reset mode left unlimited speed after reloading the baseline before episode {}.",
					s_completed_episodes + 1);
				return false;
			}

			s_successful_reset_count++;
			s_episode_emulation_start = Clock::now();
			s_episode_emulation_active = true;
			s_phase = Phase::Measuring;
			return true;
		}

		void Finalize(const Clock::time_point end_time, const bool success, std::string_view error_text)
		{
			if (s_config.mode == "reset" && s_episode_emulation_active)
			{
				s_emulation_seconds += std::chrono::duration<double>(end_time - s_episode_emulation_start).count();
				s_episode_emulation_active = false;
			}

			s_phase = Phase::Finalized;
			if (s_config.mode == "control" || s_config.mode == "reset")
				ResetControlledPorts();

			const double wall_seconds = std::chrono::duration<double>(end_time - s_measurement_start).count();
			std::string result = BuildResult(success, error_text, wall_seconds);
			std::string output_text = result;
			output_text.push_back('\n');

			if (!FileSystem::WriteStringToFile(s_config.output.c_str(), output_text))
			{
				std::string write_error = fmt::format("Failed to write RL benchmark result to '{}'.", s_config.output);
				if (!error_text.empty())
					write_error = fmt::format("{} {}", error_text, write_error);
				result = BuildResult(false, write_error, wall_seconds);
				std::fprintf(stderr, "%s\n", write_error.c_str());
			}

			std::fwrite(result.data(), 1, result.size(), stdout);
			std::fputc('\n', stdout);
			std::fflush(stdout);

			Host::RequestVMShutdown(false, false, false);
		}
	} // namespace

	bool Initialize(const std::string& config_path, Error* error)
	{
		EnsureBenchmarkStandardStreams();

		if (s_phase != Phase::Disabled)
		{
			Error::SetString(error, "RL benchmark has already been initialized.");
			return false;
		}

		const std::optional<std::string> config_text = FileSystem::ReadFileToString(config_path.c_str());
		if (!config_text.has_value())
		{
			Error::SetStringFmt(error, "Failed to read RL benchmark config '{}'.", config_path);
			return false;
		}

		rapidjson::Document document;
		document.Parse(config_text->data(), config_text->size());
		if (document.HasParseError())
		{
			Error::SetStringFmt(error, "Failed to parse RL benchmark config '{}': {} at byte {}.", config_path,
				rapidjson::GetParseError_En(document.GetParseError()), document.GetErrorOffset());
			return false;
		}
		if (!document.IsObject())
		{
			Error::SetStringFmt(error, "RL benchmark config '{}' must contain a JSON object.", config_path);
			return false;
		}

		const auto version = document.FindMember("schema_version");
		if (version == document.MemberEnd())
		{
			Error::SetString(error, "RL benchmark config is missing required field 'schema_version'.");
			return false;
		}
		if (!version->value.IsUint() || version->value.GetUint() != CONFIG_SCHEMA_VERSION)
		{
			Error::SetStringFmt(error, "Unsupported RL benchmark config schema_version; expected {}.", CONFIG_SCHEMA_VERSION);
			return false;
		}

		Config config;
		if (!ReadRequiredString(document, "mode", &config.mode, error) ||
			!ReadRequiredUInt64(document, "warmup_frames", &config.warmup_frames, error) ||
			!ReadRequiredUInt64(document, "frames", &config.frames, error) ||
			!ReadRequiredUInt64(document, "decision_interval", &config.decision_interval, error) ||
			!ReadRequiredUInt64(document, "input_seed", &config.input_seed, error) ||
			!ReadRequiredString(document, "output", &config.output, error))
		{
			return false;
		}

		if (config.mode != "raw" && config.mode != "observe" && config.mode != "control" && config.mode != "reset")
		{
			Error::SetStringFmt(error,
				"Unsupported RL benchmark mode '{}'; expected 'raw', 'observe', 'control', or 'reset'.", config.mode);
			return false;
		}
		if (config.frames == 0)
		{
			Error::SetString(error, "RL benchmark config field 'frames' must be greater than zero.");
			return false;
		}
		if (config.decision_interval == 0)
		{
			Error::SetString(error, "RL benchmark config field 'decision_interval' must be greater than zero.");
			return false;
		}
		if ((config.mode == "observe" || config.mode == "control" || config.mode == "reset") &&
			!ParseObservationRanges(document, &config, error))
		{
			return false;
		}
		if ((config.mode == "control" || config.mode == "reset") && !ParseControlPorts(document, &config, error))
			return false;

		if (config.mode == "reset")
		{
			if (!ReadRequiredUInt64(document, "episodes", &config.episodes, error) ||
				!ReadRequiredString(document, "baseline_savestate", &config.baseline_savestate, error))
			{
				return false;
			}
			if (config.episodes == 0)
			{
				Error::SetString(error, "RL benchmark reset config field 'episodes' must be greater than zero.");
				return false;
			}
			if (!FileSystem::FileExists(config.baseline_savestate.c_str()))
			{
				Error::SetStringFmt(error, "RL benchmark baseline savestate '{}' does not exist.", config.baseline_savestate);
				return false;
			}
		}

		s_config = std::move(config);
		s_environment = {};
		s_warmup_frames_seen = 0;
		s_measured_frames = 0;
		s_observation_count = 0;
		s_observation_bytes = 0;
		s_trajectory_hash = FNV1A64_OFFSET_BASIS;
		s_observation_buffer.clear();
		if (s_config.max_observation_range_size > 0)
			s_observation_buffer.resize(s_config.max_observation_range_size);
		s_control_decision_count = 0;
		s_controller_updates = {};
		s_action_sequence_hash = FNV1A64_OFFSET_BASIS;
		ResetControlGeneratorState();
		s_episode_frames = 0;
		s_episode_decision_count = 0;
		s_completed_episodes = 0;
		s_successful_reset_count = 0;
		s_failed_reset_count = 0;
		s_reset_checkpoint_count = 0;
		s_reset_checkpoint_mismatch_count = 0;
		s_reset_checkpoint_hash.reset();
		s_reset_latencies_seconds.clear();
		s_emulation_seconds = 0.0;
		s_episode_emulation_active = false;
		s_phase = Phase::Warmup;
		return true;
	}

	bool IsEnabled()
	{
		return s_phase == Phase::Warmup || s_phase == Phase::Measuring || s_phase == Phase::Resetting;
	}

	void OnVSync()
	{
		if (s_phase == Phase::Warmup)
		{
			if (s_warmup_frames_seen < s_config.warmup_frames)
			{
				s_warmup_frames_seen++;
				if (s_warmup_frames_seen < s_config.warmup_frames)
					return;
			}

			CaptureEnvironment();
			if (s_environment.limiter_mode != LimiterModeType::Unlimited)
			{
				s_measurement_start = Clock::now();
				Finalize(s_measurement_start, false,
					"RL benchmark requires unlimited speed; launch PCSX2 with -unlimited.");
				return;
			}

			if (s_config.mode == "control" || s_config.mode == "reset")
			{
				std::string controller_error;
				if (!ValidateControlPorts(&controller_error))
				{
					s_measurement_start = Clock::now();
					Finalize(s_measurement_start, false, controller_error);
					return;
				}
				ResetControlledPorts();
			}

			// Environment discovery and control-port validation are outside the timed interval.
			s_measurement_start = Clock::now();
			if (s_config.mode == "reset")
			{
				std::string reset_error;
				if (!BeginResetEpisode(&reset_error))
					Finalize(Clock::now(), false, reset_error);
				return;
			}

			s_phase = Phase::Measuring;
			return;
		}

		if (s_phase == Phase::Resetting)
			return;
		if (s_phase != Phase::Measuring)
			return;

		if (s_config.mode == "reset")
		{
			s_measured_frames++;
			s_episode_frames++;
			if ((s_episode_frames % s_config.decision_interval) == 0)
			{
				std::string control_error;
				if (!CaptureControlDecision(s_episode_frames, s_episode_decision_count, &control_error))
				{
					Finalize(Clock::now(), false, control_error);
					return;
				}
				s_episode_decision_count++;
			}

			if (s_episode_frames == s_config.frames)
			{
				const Clock::time_point episode_end = Clock::now();
				if (s_episode_emulation_active)
				{
					s_emulation_seconds +=
						std::chrono::duration<double>(episode_end - s_episode_emulation_start).count();
					s_episode_emulation_active = false;
				}
				s_completed_episodes++;

				if (VMManager::GetLimiterMode() != LimiterModeType::Unlimited)
				{
					Finalize(episode_end, false,
						"RL benchmark left unlimited speed before the reset benchmark completed.");
				}
				else if (s_completed_episodes == s_config.episodes)
				{
					Finalize(episode_end, true, {});
				}
				else
				{
					std::string reset_error;
					if (!BeginResetEpisode(&reset_error))
						Finalize(Clock::now(), false, reset_error);
				}
			}
			return;
		}

		s_measured_frames++;
		if (s_config.mode == "observe" && (s_measured_frames % s_config.decision_interval) == 0)
		{
			std::string observation_error;
			if (!CaptureObservation(s_measured_frames, &observation_error))
			{
				Finalize(Clock::now(), false, observation_error);
				return;
			}
		}
		else if (s_config.mode == "control" && (s_measured_frames % s_config.decision_interval) == 0)
		{
			std::string control_error;
			if (!CaptureControlDecision(s_measured_frames, s_control_decision_count, &control_error))
			{
				Finalize(Clock::now(), false, control_error);
				return;
			}
		}

		if (s_measured_frames == s_config.frames)
		{
			const Clock::time_point end_time = Clock::now();
			if (VMManager::GetLimiterMode() != LimiterModeType::Unlimited)
			{
				Finalize(end_time, false,
					"RL benchmark left unlimited speed before the measured run completed.");
			}
			else
			{
				Finalize(end_time, true, {});
			}
		}
	}
} // namespace RLBenchmark
