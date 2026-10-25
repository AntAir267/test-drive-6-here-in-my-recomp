/* The mixer behind the DirectSound stand-in (dsound.c).
 *
 * One source of truth for play positions: a buffer's position moves only when the mixer steps
 * it, sample by sample at the buffer's frequency against an output rate. Who drives the steps:
 *
 *  - Nothing attached (the default while there is no audio device): every DirectSound call that
 *    reads or changes playback state (GetCurrentPosition, GetStatus, Lock, Play, Stop,
 *    SetFrequency, ...) first catches all buffers up to the sound clock, as if a 44.1 kHz device
 *    had consumed them since the last call. Same stepping as sound_mix, without output.
 *
 *  - An output attached: its thread pulls sound_mix() in real time; DirectSound calls no longer
 *    move anything and the play cursor is exactly what the output has consumed. The write
 *    cursor leads it by max(15 ms, one output chunk), so the game never writes into data the
 *    mixer has already read.
 *
 * Hooking up SDL3 audio later:
 *    open a float32 stereo stream at the device rate; sound_output_attach(rate, chunk) where
 *    chunk is the most frames one callback asks for (keep it small, e.g. via
 *    SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES = 256-512, since it widens the write lead); in the
 *    stream's get-callback call sound_mix() for the requested frames and put them in the
 *    stream; sound_output_detach() when the device closes (positions go back to the clock).
 *
 * Debug aid: TD6_SOUND_WAV=<path> writes everything sound_mix produces to a 16-bit stereo
 * WAV. With no device attached it runs its own output thread pulling sound_mix in real time at
 * 44.1 kHz; once a device is attached that thread idles and the file records the device's mix.
 *
 * Deterministic tests: every time read goes through sound_clock_ms(); sound_set_clock() swaps
 * in a virtual clock (positions then depend only on the clock values seen at each call).
 */
#pragma once
#include <stdint.h>

#define SOUND_CLOCK_RATE 44100     /* rate of the virtual device used while nothing is attached */
#define SOUND_WRITE_LEAD_MS 15.0   /* how far the write cursor leads the play cursor (Windows) */

/* Mix every playing secondary buffer into out (stereo interleaved, frames x 2 floats, written
   not accumulated, clamped to -1..1) and advance their positions. Only the attached output may
   call it: with nothing attached, positions already follow the clock. */
void sound_mix(float *out, int frames, int rate);

/* An output device takes over the stepping; chunk = the largest frame count it asks for. */
void sound_output_attach(int rate, int chunk_frames);
void sound_output_detach(void);

double sound_clock_ms(void);
void sound_set_clock(double (*clock_ms)(void));     /* NULL restores now_ms */

/* For the core's CoCreateInstance: creates the object if clsid is CLSID_DirectSound (left
   uninitialized, as COM does: the program then calls IDirectSound::Initialize). Returns an
   HRESULT; REGDB_E_CLASSNOTREG (0x80040154) for any other class. */
uint32_t dsound_cocreate(uint32_t clsid, uint32_t riid, uint32_t ppv);

/* The runtime's own voices (intro videos' soundtrack): 16-bit PCM copied in, playing at once,
   full volume, centred. Returns an id for sound_stream_stop, or -1. */
int sound_stream_start(const int16_t *pcm, uint32_t frames, int channels, int rate);
void sound_stream_stop(int id);
