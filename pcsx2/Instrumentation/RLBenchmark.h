// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>

class Error;

namespace RLBenchmark
{
	/// Loads and validates a benchmark configuration. Must be called before the VM starts.
	bool Initialize(const std::string& config_path, Error* error);

	/// Returns true while a configured benchmark is waiting for or counting VSyncs.
	bool IsEnabled();

	/// Advances the benchmark lifecycle by one CPU-thread VSync boundary.
	void OnVSync();
} // namespace RLBenchmark
