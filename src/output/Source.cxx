/*
 * Copyright 2003-2017 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "Source.hxx"
#include "MusicChunk.hxx"
#include "filter/FilterInternal.hxx"
#include "filter/plugins/ReplayGainFilterPlugin.hxx"
#include "pcm/PcmMix.hxx"
#include "thread/Mutex.hxx"
#include "util/ConstBuffer.hxx"
#include "util/RuntimeError.hxx"

#include <string.h>

AudioFormat
AudioOutputSource::Open(const AudioFormat audio_format, const MusicPipe &_pipe,
			PreparedFilter *prepared_replay_gain_filter,
			PreparedFilter *prepared_other_replay_gain_filter,
			PreparedFilter *prepared_filter)
{
	assert(audio_format.IsValid());

	if (!IsOpen() || &_pipe != &pipe.GetPipe())
		pipe.Init(_pipe);

	/* (re)open the filter */

	if (filter_instance != nullptr &&
	    audio_format != in_audio_format)
		/* the filter must be reopened on all input format
		   changes */
		CloseFilter();

	if (filter_instance == nullptr)
		/* open the filter */
		OpenFilter(audio_format,
			   prepared_replay_gain_filter,
			   prepared_other_replay_gain_filter,
			   prepared_filter);

	in_audio_format = audio_format;
	return filter_instance->GetOutAudioFormat();
}

void
AudioOutputSource::Close() noexcept
{
	assert(in_audio_format.IsValid());
	in_audio_format.Clear();

	Cancel();

	CloseFilter();
}

void
AudioOutputSource::Cancel() noexcept
{
	current_chunk = nullptr;
	pipe.Cancel();

	if (replay_gain_filter_instance != nullptr)
		replay_gain_filter_instance->Reset();

	if (other_replay_gain_filter_instance != nullptr)
		other_replay_gain_filter_instance->Reset();

	if (filter_instance != nullptr)
		filter_instance->Reset();
}

void
AudioOutputSource::OpenFilter(AudioFormat audio_format,
			      PreparedFilter *prepared_replay_gain_filter,
			      PreparedFilter *prepared_other_replay_gain_filter,
			      PreparedFilter *prepared_filter)
try {
	assert(audio_format.IsValid());

	/* the replay_gain filter cannot fail here */
	if (prepared_replay_gain_filter != nullptr) {
		replay_gain_serial = 0;
		replay_gain_filter_instance =
			prepared_replay_gain_filter->Open(audio_format);
	}

	if (prepared_other_replay_gain_filter != nullptr) {
		other_replay_gain_serial = 0;
		other_replay_gain_filter_instance =
			prepared_other_replay_gain_filter->Open(audio_format);
	}

	filter_instance = prepared_filter->Open(audio_format);
} catch (...) {
	CloseFilter();
	throw;
}

void
AudioOutputSource::CloseFilter() noexcept
{
	delete replay_gain_filter_instance;
	replay_gain_filter_instance = nullptr;

	delete other_replay_gain_filter_instance;
	other_replay_gain_filter_instance = nullptr;

	delete filter_instance;
	filter_instance = nullptr;
}

ConstBuffer<void>
AudioOutputSource::GetChunkData(const MusicChunk &chunk,
				Filter *replay_gain_filter,
				unsigned *replay_gain_serial_p)
{
	assert(!chunk.IsEmpty());
	assert(chunk.CheckFormat(in_audio_format));

	ConstBuffer<void> data(chunk.data, chunk.length);

	assert(data.size % in_audio_format.GetFrameSize() == 0);

	if (!data.empty() && replay_gain_filter != nullptr) {
		replay_gain_filter_set_mode(*replay_gain_filter,
					    replay_gain_mode);

		if (chunk.replay_gain_serial != *replay_gain_serial_p &&
		    chunk.replay_gain_serial != MusicChunk::IGNORE_REPLAY_GAIN) {
			replay_gain_filter_set_info(*replay_gain_filter,
						    chunk.replay_gain_serial != 0
						    ? &chunk.replay_gain_info
						    : nullptr);
			*replay_gain_serial_p = chunk.replay_gain_serial;
		}

		data = replay_gain_filter->FilterPCM(data);
	}

	return data;
}

ConstBuffer<void>
AudioOutputSource::FilterChunk(const MusicChunk &chunk)
{
	auto data = GetChunkData(chunk, replay_gain_filter_instance,
				 &replay_gain_serial);
	if (data.empty())
		return data;

	/* cross-fade */

	if (chunk.other != nullptr) {
		auto other_data = GetChunkData(*chunk.other,
					       other_replay_gain_filter_instance,
					       &other_replay_gain_serial);
		if (other_data.empty())
			return data;

		/* if the "other" chunk is longer, then that trailer
		   is used as-is, without mixing; it is part of the
		   "next" song being faded in, and if there's a rest,
		   it means cross-fading ends here */

		if (data.size > other_data.size)
			data.size = other_data.size;

		float mix_ratio = chunk.mix_ratio;
		if (mix_ratio >= 0)
			/* reverse the mix ratio (because the
			   arguments to pcm_mix() are reversed), but
			   only if the mix ratio is non-negative; a
			   negative mix ratio is a MixRamp special
			   case */
			mix_ratio = 1.0 - mix_ratio;

		void *dest = cross_fade_buffer.Get(other_data.size);
		memcpy(dest, other_data.data, other_data.size);
		if (!pcm_mix(cross_fade_dither, dest, data.data, data.size,
			     in_audio_format.format,
			     mix_ratio))
			throw FormatRuntimeError("Cannot cross-fade format %s",
						 sample_format_to_string(in_audio_format.format));

		data.data = dest;
		data.size = other_data.size;
	}

	/* apply filter chain */

	return filter_instance->FilterPCM(data);
}

bool
AudioOutputSource::Fill(Mutex &mutex)
{
	if (current_chunk != nullptr && pending_tag == nullptr &&
	    pending_data.empty())
		pipe.Consume(*std::exchange(current_chunk, nullptr));

	if (current_chunk != nullptr)
		return true;

	current_chunk = pipe.Get();
	if (current_chunk == nullptr)
		return false;

	pending_tag = current_chunk->tag.get();

	try {
		/* release the mutex while the filter runs, because
		   that may take a while */
		const ScopeUnlock unlock(mutex);

		pending_data = pending_data.FromVoid(FilterChunk(*current_chunk));
	} catch (...) {
		current_chunk = nullptr;
		throw;
	}

	return true;
}

void
AudioOutputSource::ConsumeData(size_t nbytes) noexcept
{
	pending_data.skip_front(nbytes);

	if (pending_data.empty())
		pipe.Consume(*std::exchange(current_chunk, nullptr));
}
