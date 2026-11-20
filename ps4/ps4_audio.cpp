// The game's sound on the PS4: the audio DMA's buffers (dsp.cpp, 16-bit stereo at 32 kHz) go into a
// ring; a thread above the game's priority resamples them to 48 kHz and plays them on the main port
// in 256-frame blocks (sceAudioOutOutput blocks until the port takes one: the pacing).
// SSA_AUDIO=0 (env.txt): no sound (and the DSP doesn't mix voices).
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include "ps4_kit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <mutex>
#include <thread>

extern void (*g_audioSink)(const int16_t* lr, uint32_t frames); // dsp.cpp
void ssaPinThread(int core, int priority, const char* name);       // ps4_threads.cpp

namespace
{
constexpr uint32_t kRing = 16384;      // frames at 32 kHz (0.5 s)
constexpr uint32_t kMaxQueued = 6400;  // more than 200 ms queued: the oldest go (latency stays low)
constexpr uint32_t kTarget = 1920;     // the level kept queued: 60 ms (rate steering, below)
constexpr uint32_t kGrain = 256;       // frames per port block at 48 kHz
int16_t s_ring[kRing * 2];
uint32_t s_head = 0, s_count = 0;      // read position, frames queued
std::mutex s_lock;
uint64_t s_dropped = 0, s_starved = 0, s_pushed = 0;

void push(const int16_t* lr, uint32_t frames)
{
	std::lock_guard<std::mutex> g(s_lock);
	s_pushed += frames;
	for (uint32_t i = 0; i < frames; i++)
	{
		if (s_count == kRing)
		{
			s_head = (s_head + 1) % kRing;
			s_count--;
			s_dropped++;
		}
		const uint32_t at = (s_head + s_count) % kRing;
		s_ring[2 * at] = lr[2 * i];
		s_ring[2 * at + 1] = lr[2 * i + 1];
		s_count++;
	}
	if (s_count > kMaxQueued)
	{
		const uint32_t drop = s_count - kTarget;
		s_head = (s_head + drop) % kRing;
		s_count -= drop;
		s_dropped += drop;
	}
}

void run(int32_t port)
{
	ssaPinThread(5, 400, "audio");
	static int16_t out[kGrain * 2];
	// 32 -> 48 kHz: two input frames per three output, linear between the last two taken. The step is
	// steered by up to 0.5% so that ~60 ms stay queued (the game's 32 kHz and the port's 48 kHz come
	// from different clocks, and the game's buffers come in bursts); after running dry, playing
	// resumes once half of that is queued again (no stutter of single blocks)
	int16_t prev[2] = {0, 0}, next[2] = {0, 0};
	uint32_t pos = 0; // 16.16 position between prev and next
	uint64_t blocks = 0;
	bool refill = true;
	for (;;)
	{
		{
			std::lock_guard<std::mutex> g(s_lock);
			if (refill && s_count >= kTarget / 2)
				refill = false;
			float err = ((float)s_count - (float)kTarget) / (float)kTarget;
			err = err < -1.0f ? -1.0f : err > 1.0f ? 1.0f : err;
			const uint32_t step = (uint32_t)(65536.0f * 2.0f / 3.0f * (1.0f + 0.005f * err));
			for (uint32_t i = 0; i < kGrain; i++)
			{
				while (pos >= 0x10000)
				{
					pos -= 0x10000;
					prev[0] = next[0];
					prev[1] = next[1];
					if (s_count && !refill)
					{
						next[0] = s_ring[2 * s_head];
						next[1] = s_ring[2 * s_head + 1];
						s_head = (s_head + 1) % kRing;
						s_count--;
					}
					else
					{
						// nothing queued: fade towards silence instead of a click, then wait for the
						// queue to fill again
						next[0] = (int16_t)(next[0] * 7 / 8);
						next[1] = (int16_t)(next[1] * 7 / 8);
						s_starved++;
						refill = true;
					}
				}
				out[2 * i] = (int16_t)(prev[0] + (((int32_t)next[0] - prev[0]) * (int32_t)pos >> 16));
				out[2 * i + 1] = (int16_t)(prev[1] + (((int32_t)next[1] - prev[1]) * (int32_t)pos >> 16));
				pos += step;
			}
		}
		sceAudioOutOutput(port, out);
		if (++blocks % (48000 / kGrain * 30) == 0)
		{
			std::lock_guard<std::mutex> g(s_lock);
			fprintf(stderr, "audio: 30 s: %llu frames in, %llu dropped (late), %llu starved; %u queued\n", (unsigned long long)s_pushed,
				(unsigned long long)s_dropped, (unsigned long long)s_starved, s_count);
			s_pushed = s_dropped = s_starved = 0;
		}
	}
}
} // namespace

void ps4AudioStart()
{
	if (const char* e = getenv("SSA_AUDIO"); e && !atoi(e))
	{
		fprintf(stderr, "audio: off (SSA_AUDIO=0)\n");
		return;
	}
	sceUserServiceInitialize(nullptr);
	int r = sceAudioOutInit();
	int32_t user = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
	const int32_t port = sceAudioOutOpen(user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrain, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
	fprintf(stderr, "audio: sceAudioOutInit %x, main port %x (48 kHz stereo, %u-frame blocks)\n", (unsigned)r, (unsigned)port, kGrain);
	if (port < 0)
		return;
	g_audioSink = push;
	std::thread(run, port).detach();
}
