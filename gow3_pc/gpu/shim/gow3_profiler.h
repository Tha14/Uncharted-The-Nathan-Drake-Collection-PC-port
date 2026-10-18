// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: sampling profiler for one thread, on with GOW3_PROFILE=<seconds per report>
// (gow3_profiler.cpp).
#pragma once

namespace Gow3Profiler {

/// Samples the thread (a Windows thread id) for as long as the process runs.
void Start(unsigned long thread_id, const char* name);

/// Samples the calling thread (guest threads: the game's flip thread).
void StartCurrent(const char* name);

/// A frame took `frame_ms` (GOW3_PROFILE_STALLS=1): the samplers write what they sampled in it.
void NoteStall(double frame_ms);

} // namespace Gow3Profiler
