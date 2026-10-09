#pragma once

#include <stdint.h>

// Sound for Lua scripts, like Shadertoy's audio input: an INMP441 I2S
// microphone (SCK GPIO 0, WS GPIO 4, SD GPIO 35, on I2S0: the panels use
// I2S1), analysed into a spectrum and a waveform of BINS values 0..255.
// Runs only while scripts read it (started by the first read, stopped 2 s
// after the last one); no microphone: silence.
namespace Audio {

constexpr int BINS = 128;  // FFT bins 0 .. 8 kHz (62.5 Hz each); waveform samples

// The spectrum: magnitude in dB, -100 .. -30 dBFS mapped to 0..255, smoothed
// like the Web Audio analyser (0.8) Shadertoy reads.
void fft(uint8_t out[BINS]);
// The latest waveform: 128 = silence.
void wave(uint8_t out[BINS]);

}  // namespace Audio
