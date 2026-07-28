#include "OpenALAudioDevice/OpenALAudioStream.h"
#include "OpenALAudioDevice/OpenALAudioManager.h"
#include <AL/alext.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>

// ---------------------------------------------------------------------------
// Audio diagnostics (see the header for why this exists).
// ---------------------------------------------------------------------------
namespace {

const int AUDIO_DEBUG_RING = 192;

struct AudioDebugEntry {
	double  when = 0.0;
	char    text[168] = {0};
};

AudioDebugEntry s_ring[AUDIO_DEBUG_RING];
int  s_ringNext  = 0;
long s_ringTotal = 0;

// Every live stream, so a dump can report instantaneous state and not just
// history. Streams are created and destroyed on the audio path only.
std::set<const OpenALAudioStream *> &liveStreams()
{
	static std::set<const OpenALAudioStream *> s_live;
	return s_live;
}

double nowSeconds()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

} // namespace

void AudioDebugRecord(const char *fmt, ...)
{
	AudioDebugEntry &e = s_ring[s_ringNext];
	e.when = nowSeconds();
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e.text, sizeof(e.text), fmt, ap);
	va_end(ap);
	s_ringNext = (s_ringNext + 1) % AUDIO_DEBUG_RING;
	++s_ringTotal;

	// GX_AUDIO_TRACE=1 mirrors every event to stderr as it happens, so a fault can
	// be studied from the log alone without anyone being present to hear it and
	// trigger a dump. Off by default: these fire often enough to be noise.
	static const int traceEnabled = (getenv("GX_AUDIO_TRACE") != nullptr) ? 1 : 0;
	if (traceEnabled) {
		fprintf(stderr, "[audio] %s\n", e.text);
	}
}

void AudioDebugDump(const char *reason)
{
	const double now = nowSeconds();
	fprintf(stderr, "\n===== AUDIO MARKER: %s =====\n", reason ? reason : "(none)");
	fprintf(stderr, "recorded %ld event(s); showing the most recent %d, oldest first\n",
	        s_ringTotal, (int)(s_ringTotal < AUDIO_DEBUG_RING ? s_ringTotal : AUDIO_DEBUG_RING));

	const int count = (int)(s_ringTotal < AUDIO_DEBUG_RING ? s_ringTotal : AUDIO_DEBUG_RING);
	const int start = (int)((s_ringTotal < AUDIO_DEBUG_RING) ? 0
	                        : (s_ringNext % AUDIO_DEBUG_RING));
	for (int i = 0; i < count; ++i) {
		const AudioDebugEntry &e = s_ring[(start + i) % AUDIO_DEBUG_RING];
		// Negative age = how long before the marker this happened. That relative
		// figure is the useful one: a fault heard "just now" is a few hundred ms back.
		fprintf(stderr, "  [%8.3fs] %s\n", e.when - now, e.text);
	}

	fprintf(stderr, "--- live streams: %d ---\n", (int)liveStreams().size());
	for (const OpenALAudioStream *s : liveStreams()) {
		if (s == nullptr) continue;
		const ALuint src = s->getSource();
		ALint state = 0, queued = 0, processed = 0;
		alGetSourcei(src, AL_SOURCE_STATE, &state);
		alGetSourcei(src, AL_BUFFERS_QUEUED, &queued);
		alGetSourcei(src, AL_BUFFERS_PROCESSED, &processed);
		const char *stateName =
			state == AL_PLAYING ? "PLAYING" :
			state == AL_PAUSED  ? "PAUSED"  :
			state == AL_STOPPED ? "STOPPED" :
			state == AL_INITIAL ? "INITIAL" : "?";
		fprintf(stderr, "  src=%u state=%-7s queued=%2d processed=%2d atEof=%d\n",
		        src, stateName, (int)queued, (int)processed, s->isAtEnd() ? 1 : 0);
	}
	fprintf(stderr, "===== END AUDIO MARKER =====\n\n");
	fflush(stderr);
}

OpenALAudioStream::OpenALAudioStream()
{
    liveStreams().insert(this);
    alGenSources(1, &m_source);
    alGenBuffers(AL_STREAM_BUFFER_COUNT, m_buffers);

    // GeneralsX @bugfix BenderAI 22/04/2026 Force video stream source to non-positional direct playback.
    alSourcei(m_source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(m_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSource3f(m_source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    alSourcef(m_source, AL_ROLLOFF_FACTOR, 0.0f);
    alSourcef(m_source, AL_GAIN, 1.0f);
    alSourcei(m_source, AL_LOOPING, AL_FALSE);
#ifdef AL_DIRECT_CHANNELS_SOFT
    alSourcei(m_source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
#endif
#ifdef AL_SOURCE_SPATIALIZE_SOFT
    alSourcei(m_source, AL_SOURCE_SPATIALIZE_SOFT, AL_FALSE);
#endif

    DEBUG_LOG(("OpenALAudioStream created: %i\n", m_source));
}

OpenALAudioStream::~OpenALAudioStream()
{
    liveStreams().erase(this);
    DEBUG_LOG(("OpenALAudioStream freed: %i\n", m_source));
    // Unbind the buffers first
    alSourceStop(m_source);
    alSourcei(m_source, AL_BUFFER, 0);
    alDeleteSources(1, &m_source);
    // Now delete the buffers
    alDeleteBuffers(AL_STREAM_BUFFER_COUNT, m_buffers);
}

bool OpenALAudioStream::bufferData(uint8_t *data, size_t data_size, ALenum format, int samplerate)
{
    DEBUG_LOG(("Buffering %zu bytes of data (samplerate: %i, format: %i)\n", data_size, samplerate, format));
    ALint num_queued;
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &num_queued);
    if (num_queued >= AL_STREAM_BUFFER_COUNT) {
        DEBUG_LOG(("Having too many buffers already queued: %i", num_queued));
        return false;
    }

    ALuint &current_buffer = m_buffers[m_current_buffer_idx];
    // GeneralsX @bugfix BenderAI 22/04/2026 Detect and reject invalid OpenAL buffer/queue operations.
    //
    // GeneralsX @bugfix Claude 28/07/2026 This drain was UNBOUNDED, and alGetError() does not
    // always clear. With no current context OpenAL Soft returns AL_INVALID_OPERATION from every
    // call - and logs a warning per call - so `while (alGetError() != AL_NO_ERROR) {}` never
    // terminates. It spins at 100% CPU inside the formatter for that warning.
    //
    // That is a real hang, not a theoretical one. A headless Linux host froze here, reached by
    // MultiPlayerLoadScreen::init -> OpenALAudioManager::update while loading the map, and never
    // simulated a single frame. It presented as "the match never starts", which reads exactly
    // like a network fault and cost a session to find. It is latent on EVERY platform, not a
    // Linux quirk: any context that is lost or never created - a headless run, or an output
    // device disconnected mid-game - freezes the whole process the same way.
    //
    // The error state is depth-1 per the spec (alGetError returns AND clears), so a handful of
    // passes is already generous. Anything that will not clear means there is no usable context,
    // and every AL call below would be a silent no-op, so fail the buffer instead of spinning.
    // The caller already handles that: OpenALAudioStream::update's refill loop breaks on its
    // `refreshedQueued <= num_queued` guard, so the stream degrades to silence and the engine
    // keeps running.
    const int maxErrorDrain = 16;
    for (int drained = 0; alGetError() != AL_NO_ERROR; ++drained) {
        if (drained >= maxErrorDrain) {
            DEBUG_LOG(("OpenALAudioStream::bufferData: AL error state will not clear (no current "
                "context?) - dropping this buffer\n"));
            return false;
        }
    }

    alBufferData(current_buffer, format, data, data_size, samplerate);
    ALenum err = alGetError();
    if (err != AL_NO_ERROR) {
        DEBUG_LOG(("OpenALAudioStream::bufferData alBufferData failed: err=0x%x format=0x%x size=%zu rate=%d\n",
            (unsigned int)err, (unsigned int)format, data_size, samplerate));
        AudioDebugRecord("src=%u alBufferData FAILED err=0x%x format=0x%x size=%zu rate=%d",
                         m_source, (unsigned int)err, (unsigned int)format, data_size, samplerate);
        return false;
    }

    // Record what actually reaches the device. A burst of noise is either stale
    // audio replayed (handled above) or a buffer whose contents/parameters are
    // wrong, and the two are indistinguishable from restart events alone. Peak
    // amplitude is the discriminator: normal speech and music sit well below full
    // scale, so a buffer pinned at the rail is garbage or a format mismatch.
    {
        int peak = 0;
        if (format == AL_FORMAT_MONO16 || format == AL_FORMAT_STEREO16) {
            const int16_t *s = reinterpret_cast<const int16_t *>(data);
            const size_t n = data_size / sizeof(int16_t);
            for (size_t i = 0; i < n; ++i) {
                int v = s[i] < 0 ? -s[i] : s[i];
                if (v > peak) peak = v;
            }
            peak = (peak * 100) / 32768;   // percent of full scale
        }
        // Log the source's ACTUAL gain next to the peak. Loudness heard by the
        // player is peak * gain, so the two together separate "the sample itself
        // is hot" from "this is playing louder than it should". Also flag the
        // source's own reported volume so a mis-set gain is visible directly.
        ALfloat gain = -1.0f;
        alGetSourcef(m_source, AL_GAIN, &gain);
        static int s_bufSeq = 0;
        const bool loudOnset = (peak >= 90 && gain >= 0.85f);
        if (loudOnset || peak >= 97 || (++s_bufSeq % 64) == 0) {
            AudioDebugRecord("src=%u buffered size=%zu fmt=0x%x rate=%d peak=%d%% gain=%.2f%s",
                             m_source, data_size, (unsigned int)format, samplerate, peak, gain,
                             loudOnset ? "  <-- LOUD (peak*gain high)" :
                             (peak >= 97 ? "  <-- CLIPPING" : ""));
        }
    }

    alSourceQueueBuffers(m_source, 1, &current_buffer);
    err = alGetError();
    if (err != AL_NO_ERROR) {
        DEBUG_LOG(("OpenALAudioStream::bufferData alSourceQueueBuffers failed: err=0x%x source=%u buffer=%u\n",
            (unsigned int)err, (unsigned int)m_source, (unsigned int)current_buffer));
        return false;
    }

    m_current_buffer_idx++;

    if (m_current_buffer_idx >= AL_STREAM_BUFFER_COUNT)
        m_current_buffer_idx = 0;

    return true;
}

void OpenALAudioStream::update()
{
    ALint sourceState;
    alGetSourcei(m_source, AL_SOURCE_STATE, &sourceState);

    ALint num_queued;
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &num_queued);

    // GeneralsX @bugfix 14/06/2026 EOF probe — runs BEFORE the restart-on-stopped guard below.
    // If the source has stopped having fully played everything queued (no unplayed buffers left),
    // probe the source stream once: at true EOF, latch m_endOfData so the guard does NOT restart
    // it. Without this, a one-shot voice line that ends while its queue is still over the refill
    // threshold (so the periodic refill/EOF check below hasn't run) gets restarted, REPLAYING its
    // already-played buffers as a repeating 'chip' until the next line. If data IS still available
    // this is a genuine underrun and m_endOfData stays false so the normal refill+restart recovers.
    {
        ALint processedNow = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processedNow);
        if (sourceState == AL_STOPPED && num_queued > 0 && processedNow >= num_queued
            && !m_endOfData && m_requireDataCallback) {
            ALint queuedBefore = num_queued;
            bool moreData = m_requireDataCallback();
            ALint queuedAfter = 0;
            alGetSourcei(m_source, AL_BUFFERS_QUEUED, &queuedAfter);
            if (!moreData) {
                m_endOfData = true;   // definitive EOF from the decoder
            }
            else if (queuedAfter <= queuedBefore) {
                // GeneralsX @bugfix 04/07/2026 The decoder claims more data is coming but
                // produced none. Decode here is synchronous, so a persistently failing
                // packet never heals — and without this, the restart guard below replays
                // the already-played queue forever: the audible "chirping" loop after a
                // voice line, which also pins the stream un-stopped and holds the
                // disallow-speech flag (silencing subsequent EVA). Three consecutive
                // no-growth probes = the stream is done; latch EOF and let it stop.
                if (++m_stalledProbes >= 3) {
                    m_endOfData = true;
                }
            }
            else {
                m_stalledProbes = 0;
            }
        }
    }

    // GeneralsX @bugfix BenderAI 22/04/2026 Restart before unqueue to avoid dropping freshly queued
    // briefing buffers when OpenAL reports AL_STOPPED with processed buffers.
    // GeneralsX @bugfix 14/06/2026 ...but NOT once the stream is at true EOF: a finished one-shot
    // speech (taunt) must be allowed to reach a stable AL_STOPPED so its disallowSpeech flag clears.
    if ((sourceState == AL_STOPPED || sourceState == AL_INITIAL || sourceState == AL_PAUSED) && num_queued > 0 && !m_endOfData) {
        // Only restart if some queued buffer has NOT been played yet.
        //
        // num_queued > 0 is not enough: a drained source still reports its spent
        // buffers as queued until they are unqueued, so this fired with
        // processed == queued and OpenAL replayed the whole queue from the start.
        // Traced on macOS as "queued=16 processed=16" — sixteen buffers of
        // already-played audio dumped out at once, heard as a burst of noise.
        ALint alreadyProcessed = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &alreadyProcessed);
        AudioDebugRecord("src=%u restart-on-stopped (pre-unqueue) state=%d queued=%d processed=%d%s",
                         m_source, (int)sourceState, (int)num_queued, (int)alreadyProcessed,
                         (alreadyProcessed >= num_queued) ? " SKIPPED(all-spent)" : "");
        if (alreadyProcessed < num_queued) {
            play();
            alGetSourcei(m_source, AL_SOURCE_STATE, &sourceState);
        }
    }

    ALint processedBeforeUnqueue = 0;
    alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processedBeforeUnqueue);
    DEBUG_LOG(("%i buffers have been processed\n", processedBeforeUnqueue));

    // GeneralsX @bugfix BenderAI 22/04/2026 Only unqueue processed data in active playback states.
    ALint processedToUnqueue = ((sourceState == AL_PLAYING || sourceState == AL_PAUSED) ? processedBeforeUnqueue : 0);

    // GeneralsX @bugfix Also reap a stopped source whose queue is entirely spent.
    //
    // The state gate above assumed a stopped source would be restarted into
    // AL_PLAYING moments later and reaped on the next pass. Now that an all-spent
    // queue is deliberately NOT restarted (it would replay old audio), nothing ever
    // put such a source back into an active state, so its buffers accumulated —
    // observed climbing queued=13,14,15,…  Unqueueing here is safe precisely
    // because every buffer has been played: there is no fresh data to discard.
    if (processedToUnqueue == 0 && processedBeforeUnqueue > 0) {
        ALint queuedNow = 0;
        alGetSourcei(m_source, AL_BUFFERS_QUEUED, &queuedNow);
        if (processedBeforeUnqueue >= queuedNow) {
            processedToUnqueue = processedBeforeUnqueue;
        }
    }

    while (processedToUnqueue > 0) {
        ALuint buffer;
        alSourceUnqueueBuffers(m_source, 1, &buffer);
        processedToUnqueue--;
    }

    // GeneralsX @bugfix 14/06/2026 At true EOF the source has stopped with its final buffers
    // still queued-but-processed; the state-gated unqueue above skips them. Reap them here so
    // num_queued can reach 0, letting processPlayingList detect the finished one-shot as stopped
    // (which clears disallowSpeech the frame the audio ends, so back-to-back taunts play).
    if (m_endOfData) {
        ALint processedAtEof = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processedAtEof);
        while (processedAtEof > 0) {
            ALuint buffer;
            alSourceUnqueueBuffers(m_source, 1, &buffer);
            processedAtEof--;
        }
    }

    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &num_queued);
    DEBUG_LOG(("Having %i buffers queued\n", num_queued));

    if (num_queued < AL_STREAM_BUFFER_COUNT / 2 && m_requireDataCallback && !m_endOfData) {
        // GeneralsX @bugfix BenderAI 22/04/2026 Do not fake queue growth when callback fails to enqueue data.
        // Ask for more data to be buffered.
        // Only fill up to the half, because some formats can output
        // more than one buffer per decoded frame.
        while (num_queued < AL_STREAM_BUFFER_COUNT / 2) {
            // GeneralsX @bugfix 14/06/2026 callback returns FALSE at true end-of-file (no more
            // data will ever come). Latch m_endOfData so we stop restarting the drained source
            // and let it finish. A transient decode error still returns TRUE, so a stutter/underrun
            // mid-line is NOT mistaken for the end and the existing recovery path runs unchanged.
            if (!m_requireDataCallback()) {
                m_endOfData = true;
                break;
            }

            ALint refreshedQueued = 0;
            alGetSourcei(m_source, AL_BUFFERS_QUEUED, &refreshedQueued);
            if (refreshedQueued <= num_queued) {
                break;
            }
            num_queued = refreshedQueued;
            m_stalledProbes = 0;  // GeneralsX @bugfix 04/07/2026 healthy refill: stalls must be CONSECUTIVE to latch EOF, not accumulated over the stream's lifetime
        }
    }

    // GeneralsX @bugfix fbraz3 27/04/2026 Restart after refill when a generic speech stream
    // began the frame with an empty queue; otherwise processPlayingList() can release it as
    // stopped before the newly buffered narrator audio ever starts playing.
    // GeneralsX @bugfix 14/06/2026 As above, do not restart a source that has reached true EOF.
    alGetSourcei(m_source, AL_SOURCE_STATE, &sourceState);
    if ((sourceState == AL_STOPPED || sourceState == AL_INITIAL || sourceState == AL_PAUSED) && num_queued > 0 && !m_endOfData) {
        // Same guard as the pre-unqueue restart above: a queue whose buffers have
        // all been played holds nothing new to hear, and restarting it replays the
        // lot. This site was observed doing exactly that with queued=16 processed=16.
        // Re-read the queue depth here. num_queued was sampled before the refill
        // above, so comparing it against a freshly-read processed count made a
        // source that had just been handed new data look "all spent" — the restart
        // was skipped and the new audio never played.
        ALint processedNow = 0;
        ALint queuedNowAfterRefill = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processedNow);
        alGetSourcei(m_source, AL_BUFFERS_QUEUED, &queuedNowAfterRefill);
        AudioDebugRecord("src=%u restart-after-refill state=%d queued=%d processed=%d%s",
                         m_source, (int)sourceState, (int)queuedNowAfterRefill, (int)processedNow,
                         (processedNow >= queuedNowAfterRefill) ? " SKIPPED(all-spent)" : "");
        if (processedNow < queuedNowAfterRefill) {
            play();
        }
    }
}

void OpenALAudioStream::reset()
{
    DEBUG_LOG(("Resetting stream\n"));
    // alSourceStop() marks all queued buffers as processed so they can be
    // unqueued. alSourceRewind() transitions to AL_INITIAL but does NOT move
    // unprocessed buffers to processed state, so the subsequent
    // alSourcei(AL_BUFFER, 0) would fail with AL_INVALID_OPERATION if any
    // buffers were still pending.
    alSourceStop(m_source);
    ALint num_queued;
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &num_queued);
    while (num_queued > 0) {
        ALuint buf;
        alSourceUnqueueBuffers(m_source, 1, &buf);
        num_queued--;
    }
    m_current_buffer_idx = 0;
    m_endOfData = false;  // GeneralsX @bugfix 14/06/2026 streams are reused (handleToKill/replace); clear EOF latch
    m_stalledProbes = 0;
}

bool OpenALAudioStream::isPlaying()
{
    ALint state;
    alGetSourcei(m_source, AL_SOURCE_STATE, &state);
    return state == AL_PLAYING;
}