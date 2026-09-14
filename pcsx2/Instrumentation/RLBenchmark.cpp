// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Instrumentation/RLBenchmark.h"

#include "BuildVersion.h"
#include "Host.h"
#include "VMManager.h"

#include "common/Error.h"
#include "common/FileSystem.h"

#include "fmt/format.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <utility>

namespace RLBenchmark
{
	namespace
	{
		constexpr unsigned int CONFIG_SCHEMA_VERSION = 1;
		constexpr unsigned int RESULT_SCHEMA_VERSION = 1;

		struct Config
		{
			std::string mode;
			std::uint64_t warmup_frames = 0;
			std::uint64_t frames = 0;
			std::uint64_t decision_interval = 0;
			std::uint64_t input_seed = 0;
			std::string output;
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
		Phase s_phase = Phase::Disabled;
		std::uint64_t s_warmup_frames_seen = 0;
		std::uint64_t s_measured_frames = 0;
		Clock::time_point s_measurement_start;

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

			AddNullableStringMember(document, "pcsx2_build", BuildVersion::GitRev);
			AddNullableStringMember(document, "pcsx2_commit", BuildVersion::GitHash);
			document.AddMember("success", success, allocator);
			AddStringMember(document, "error", error_text);

			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			document.Accept(writer);
			return std::string(buffer.GetString(), buffer.GetSize());
		}

		void Finalize(const Clock::time_point end_time)
		{
			s_phase = Phase::Finalized;

			const double wall_seconds = std::chrono::duration<double>(end_time - s_measurement_start).count();
			std::string result = BuildResult(true, {}, wall_seconds);
			std::string output_text = result;
			output_text.push_back('\n');

			if (!FileSystem::WriteStringToFile(s_config.output.c_str(), output_text))
			{
				const std::string error_text = fmt::format("Failed to write RL benchmark result to '{}'.", s_config.output);
				result = BuildResult(false, error_text, wall_seconds);
				std::fprintf(stderr, "%s\n", error_text.c_str());
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

		s_config = std::move(config);
		s_warmup_frames_seen = 0;
		s_measured_frames = 0;
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

			// A VSync boundary is the baseline for timing complete emulated frame intervals.
			s_measurement_start = Clock::now();
			s_phase = Phase::Measuring;
			return;
		}

		if (s_phase != Phase::Measuring)
			return;

		s_measured_frames++;
		if (s_measured_frames == s_config.frames)
			Finalize(Clock::now());
	}
} // namespace RLBenchmark
