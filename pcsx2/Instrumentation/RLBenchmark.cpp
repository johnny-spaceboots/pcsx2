// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Instrumentation/RLBenchmark.h"

#include "BuildVersion.h"
#include "DebugTools/DebugInterface.h"
#include "GS/GS.h"
#include "Host.h"
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
		constexpr unsigned int RESULT_SCHEMA_VERSION = 3;
		constexpr std::uint64_t TRAJECTORY_HASH_OFFSET_BASIS = 14695981039346656037ull;
		constexpr std::uint64_t TRAJECTORY_HASH_PRIME = 1099511628211ull;

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

		enum class Phase
		{
			Disabled,
			Warmup,
			Measuring,
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
		std::uint64_t s_trajectory_hash = TRAJECTORY_HASH_OFFSET_BASIS;
		std::vector<u8> s_observation_buffer;
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

			// PCSX2 is a Windows-subsystem executable, so its CRT standard streams are not
			// connected to the invoking terminal by default. Attach to the parent console
			// for benchmark CLI output, while preserving any already-valid redirections.
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
				Error::SetString(error, "RL benchmark observe mode requires 'observation_ranges'.");
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
			*hash *= TRAJECTORY_HASH_PRIME;
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

		bool CaptureObservation(const std::uint64_t measured_frame, std::string* error_text)
		{
			DebugInterface& ee = DebugInterface::get(BREAKPOINT_EE);
			const std::uint64_t decision_index = s_observation_count;
			std::uint64_t next_hash = s_trajectory_hash;
			HashUInt64(&next_hash, measured_frame);
			HashUInt64(&next_hash, decision_index);

			for (std::size_t i = 0; i < s_config.observation_ranges.size(); i++)
			{
				const ObservationRange& range = s_config.observation_ranges[i];
				if (!ee.ReadBytes(range.address, s_observation_buffer.data(), range.size))
				{
					*error_text = fmt::format(
						"RL benchmark observe mode failed to read observation range {} at EE address 0x{:08X} ({} bytes) "
						"on measured frame {} (decision {}).",
						i, range.address, range.size, measured_frame, decision_index);
					return false;
				}

				s_observation_bytes += range.size;
				HashBytes(&next_hash, s_observation_buffer.data(), range.size);
			}

			s_trajectory_hash = next_hash;
			s_observation_count++;
			return true;
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
			document.AddMember(
				"emulated_fps", wall_seconds > 0.0 ? static_cast<double>(s_measured_frames) / wall_seconds : 0.0, allocator);

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
			document.AddMember("synthetic_input_updates", 0u, allocator);

			if (s_config.mode == "observe")
			{
				AddStringMember(document, "trajectory_hash_algorithm", "fnv1a64");
				AddStringMember(document, "trajectory_hash", fmt::format("{:016X}", s_trajectory_hash));
			}
			else
			{
				document.AddMember("trajectory_hash_algorithm", rapidjson::Value(rapidjson::kNullType), allocator);
				document.AddMember("trajectory_hash", rapidjson::Value(rapidjson::kNullType), allocator);
			}

			document.AddMember("success", success, allocator);
			AddStringMember(document, "error", error_text);

			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			document.Accept(writer);
			return std::string(buffer.GetString(), buffer.GetSize());
		}

		void Finalize(const Clock::time_point end_time, const bool success, std::string_view error_text)
		{
			s_phase = Phase::Finalized;

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

			// On the CPU thread this stops the VM immediately; batch/no-gui mode then exits the application.
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

		if (config.mode != "raw" && config.mode != "observe")
		{
			Error::SetStringFmt(error, "Unsupported RL benchmark mode '{}'; expected 'raw' or 'observe'.", config.mode);
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
		if (config.mode == "observe" && !ParseObservationRanges(document, &config, error))
			return false;

		s_config = std::move(config);
		s_environment = {};
		s_warmup_frames_seen = 0;
		s_measured_frames = 0;
		s_observation_count = 0;
		s_observation_bytes = 0;
		s_trajectory_hash = TRAJECTORY_HASH_OFFSET_BASIS;
		s_observation_buffer.clear();
		if (s_config.max_observation_range_size > 0)
			s_observation_buffer.resize(s_config.max_observation_range_size);
		s_phase = Phase::Warmup;
		return true;
	}

	bool IsEnabled()
	{
		return s_phase == Phase::Warmup || s_phase == Phase::Measuring;
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

			// A VSync boundary is the baseline for timing complete emulated frame intervals.
			// Environment discovery above is intentionally outside the measured interval.
			s_measurement_start = Clock::now();
			s_phase = Phase::Measuring;
			return;
		}

		if (s_phase != Phase::Measuring)
			return;

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
