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

#ifndef MPD_DECODER_CONTROL_HXX
#define MPD_DECODER_CONTROL_HXX

#include "DecoderCommand.hxx"
#include "AudioFormat.hxx"
#include "MixRampInfo.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "thread/Thread.hxx"
#include "Chrono.hxx"
#include "ReplayGainConfig.hxx"
#include "ReplayGainMode.hxx"

#include <exception>
#include <utility>
#include <memory>

#include <assert.h>
#include <stdint.h>

/* damn you, windows.h! */
#ifdef ERROR
#undef ERROR
#endif

class DetachedSong;
class MusicBuffer;
class MusicPipe;

enum class DecoderState : uint8_t {
	STOP = 0,
	START,
	DECODE,

	/**
	 * The last "START" command failed, because there was an I/O
	 * error or because no decoder was able to decode the file.
	 * This state will only come after START; once the state has
	 * turned to DECODE, by definition no such error can occur.
	 */
	ERROR,
};

struct DecoderControl {
	/**
	 * The handle of the decoder thread.
	 */
	Thread thread;

	/**
	 * This lock protects #state and #command.
	 *
	 * This is usually a reference to PlayerControl::mutex, so
	 * that both player thread and decoder thread share a mutex.
	 * This simplifies synchronization with #cond and
	 * #client_cond.
	 */
	Mutex &mutex;

	/**
	 * Trigger this object after you have modified #command.  This
	 * is also used by the decoder thread to notify the caller
	 * when it has finished a command.
	 */
	Cond cond;

	/**
	 * The trigger of this object's client.  It is signalled
	 * whenever an event occurs.
	 *
	 * This is usually a reference to PlayerControl::cond.
	 */
	Cond &client_cond;

	DecoderState state = DecoderState::STOP;
	DecoderCommand command = DecoderCommand::NONE;

	/**
	 * The error that occurred in the decoder thread.  This
	 * attribute is only valid if #state is #DecoderState::ERROR.
	 * The object must be freed when this object transitions to
	 * any other state (usually #DecoderState::START).
	 */
	std::exception_ptr error;

	bool quit;

	/**
	 * Is the client currently waiting for the DecoderThread?  If
	 * false, the DecoderThread may omit invoking Cond::signal(),
	 * reducing the number of system calls.
	 */
	bool client_is_waiting = false;

	bool seek_error;
	bool seekable;
	SongTime seek_time;

	/**
	 * The "audio_output_format" setting.
	 */
	const AudioFormat configured_audio_format;

	/** the format of the song file */
	AudioFormat in_audio_format;

	/** the format being sent to the music pipe */
	AudioFormat out_audio_format;

	/**
	 * The song currently being decoded.  This attribute is set by
	 * the player thread, when it sends the #DecoderCommand::START
	 * command.
	 */
	std::unique_ptr<DetachedSong> song;

	/**
	 * The initial seek position, e.g. to the start of a sub-track
	 * described by a CUE file.
	 *
	 * This attribute is set by Start().
	 */
	SongTime start_time;

	/**
	 * The decoder will stop when it reaches this position.  0
	 * means don't stop before the end of the file.
	 *
	 * This attribute is set by Start().
	 */
	SongTime end_time;

	SignedSongTime total_time;

	/** the #MusicChunk allocator */
	MusicBuffer *buffer;

	/**
	 * The destination pipe for decoded chunks.  The caller thread
	 * owns this object, and is responsible for freeing it.
	 */
	MusicPipe *pipe;

	const ReplayGainConfig replay_gain_config;
	ReplayGainMode replay_gain_mode = ReplayGainMode::OFF;

	float replay_gain_db = 0;
	float replay_gain_prev_db = 0;

	MixRampInfo mix_ramp, previous_mix_ramp;

	/**
	 * @param _mutex see #mutex
	 * @param _client_cond see #client_cond
	 */
	DecoderControl(Mutex &_mutex, Cond &_client_cond,
		       const AudioFormat _configured_audio_format,
		       const ReplayGainConfig &_replay_gain_config) noexcept;
	~DecoderControl() noexcept;

	/**
	 * Locks the object.
	 */
	void Lock() const noexcept {
		mutex.lock();
	}

	/**
	 * Unlocks the object.
	 */
	void Unlock() const noexcept {
		mutex.unlock();
	}

	/**
	 * Signals the object.  This function is only valid in the
	 * player thread.  The object should be locked prior to
	 * calling this function.
	 */
	void Signal() noexcept {
		cond.signal();
	}

	/**
	 * Waits for a signal on the #DecoderControl object.  This function
	 * is only valid in the decoder thread.  The object must be locked
	 * prior to calling this function.
	 */
	void Wait() noexcept {
		cond.wait(mutex);
	}

	/**
	 * Waits for a signal from the decoder thread.  This object
	 * must be locked prior to calling this function.  This method
	 * is only valid in the player thread.
	 *
	 * Caller must hold the lock.
	 */
	void WaitForDecoder() noexcept;

	bool IsIdle() const noexcept {
		return state == DecoderState::STOP ||
			state == DecoderState::ERROR;
	}

	gcc_pure
	bool LockIsIdle() const noexcept {
		const std::lock_guard<Mutex> protect(mutex);
		return IsIdle();
	}

	bool IsStarting() const noexcept {
		return state == DecoderState::START;
	}

	gcc_pure
	bool LockIsStarting() const noexcept {
		const std::lock_guard<Mutex> protect(mutex);
		return IsStarting();
	}

	bool HasFailed() const noexcept {
		assert(command == DecoderCommand::NONE);

		return state == DecoderState::ERROR;
	}

	gcc_pure
	bool LockHasFailed() const noexcept {
		const std::lock_guard<Mutex> protect(mutex);
		return HasFailed();
	}

	/**
	 * Transition this obejct from DecoderState::START to
	 * DecoderState::DECODE.
	 *
	 * Caller must lock the object.
	 */
	void SetReady(const AudioFormat audio_format,
		      bool _seekable, SignedSongTime _duration) noexcept;

	/**
	 * Checks whether an error has occurred, and if so, rethrows
	 * it.
	 *
	 * Caller must lock the object.
	 */
	void CheckRethrowError() const {
		assert(command == DecoderCommand::NONE);
		assert(state != DecoderState::ERROR || error);

		if (state == DecoderState::ERROR)
			std::rethrow_exception(error);
	}

	/**
	 * Like CheckRethrowError(), but locks and unlocks the object.
	 */
	void LockCheckRethrowError() const {
		const std::lock_guard<Mutex> protect(mutex);
		CheckRethrowError();
	}

	/**
	 * Clear the error condition (if any).
	 *
	 * Caller must lock the object.
	 */
	void ClearError() noexcept {
		if (state == DecoderState::ERROR) {
			error = std::exception_ptr();
			state = DecoderState::STOP;
		}
	}

	/**
	 * Check if the specified song is currently being decoded.  If the
	 * decoder is not running currently (or being started), then this
	 * function returns false in any case.
	 *
	 * Caller must lock the object.
	 */
	gcc_pure
	bool IsCurrentSong(const DetachedSong &_song) const noexcept;

	gcc_pure
	bool LockIsCurrentSong(const DetachedSong &_song) const noexcept {
		const std::lock_guard<Mutex> protect(mutex);
		return IsCurrentSong(_song);
	}

private:
	/**
	 * Wait for the command to be finished by the decoder thread.
	 *
	 * To be called from the client thread.  Caller must lock the
	 * object.
	 */
	void WaitCommandLocked() noexcept {
		while (command != DecoderCommand::NONE)
			WaitForDecoder();
	}

	/**
	 * Send a command to the decoder thread and synchronously wait
	 * for it to finish.
	 *
	 * To be called from the client thread.  Caller must lock the
	 * object.
	 */
	void SynchronousCommandLocked(DecoderCommand cmd) noexcept {
		command = cmd;
		Signal();
		WaitCommandLocked();
	}

	/**
	 * Send a command to the decoder thread and synchronously wait
	 * for it to finish.
	 *
	 * To be called from the client thread.  This method locks the
	 * object.
	 */
	void LockSynchronousCommand(DecoderCommand cmd) noexcept {
		const std::lock_guard<Mutex> protect(mutex);
		ClearError();
		SynchronousCommandLocked(cmd);
	}

	void LockAsynchronousCommand(DecoderCommand cmd) noexcept {
		const std::lock_guard<Mutex> protect(mutex);
		command = cmd;
		Signal();
	}

public:
	/**
	 * Marks the current command as "finished" and notifies the
	 * client (= player thread).
	 *
	 * To be called from the decoder thread.  Caller must lock the
	 * mutex.
	 */
	void CommandFinishedLocked() noexcept {
		assert(command != DecoderCommand::NONE);

		command = DecoderCommand::NONE;
		client_cond.signal();
	}

	/**
	 * Start the decoder.
	 *
	 * @param song the song to be decoded; the given instance will be
	 * owned and freed by the decoder
	 * @param start_time see #DecoderControl
	 * @param end_time see #DecoderControl
	 * @param pipe the pipe which receives the decoded chunks (owned by
	 * the caller)
	 */
	void Start(std::unique_ptr<DetachedSong> song,
		   SongTime start_time, SongTime end_time,
		   MusicBuffer &buffer, MusicPipe &pipe) noexcept;

	void Stop() noexcept;

	/**
	 * Throws #std::runtime_error on error.
	 */
	void Seek(SongTime t);

	void Quit() noexcept;

	const char *GetMixRampStart() const noexcept {
		return mix_ramp.GetStart();
	}

	const char *GetMixRampEnd() const noexcept {
		return mix_ramp.GetEnd();
	}

	const char *GetMixRampPreviousEnd() const noexcept {
		return previous_mix_ramp.GetEnd();
	}

	void SetMixRamp(MixRampInfo &&new_value) noexcept {
		mix_ramp = std::move(new_value);
	}

	/**
	 * Move mixramp_end to mixramp_prev_end and clear
	 * mixramp_start/mixramp_end.
	 */
	void CycleMixRamp() noexcept;

private:
	void RunThread() noexcept;
};

#endif
