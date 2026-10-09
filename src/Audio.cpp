#include "Audio.hpp"

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#include "Log.hpp"

namespace {

constexpr i2s_port_t PORT = I2S_NUM_0;
constexpr int PIN_SCK = 0, PIN_WS = 4, PIN_SD = 35;
constexpr int RATE = 16000;
constexpr int N = 2 * Audio::BINS;  // FFT size: 256 samples, 16 ms
constexpr int CHUNK = 64;           // stereo frames per read
constexpr float SMOOTHING = 0.8f;   // Web Audio's default
constexpr float MIN_DB = -100, MAX_DB = -30;
constexpr uint32_t IDLE_MS = 2000;  // no read for that long: stop

// shared with Lua (its task reads, ours writes)
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
uint8_t fft_out[Audio::BINS];
uint8_t wave_out[Audio::BINS];
volatile uint32_t last_read = 0;
TaskHandle_t task = nullptr;  // set while running

// iterative radix-2 FFT, in place
void fft(float* re, float* im, int n) {
	for (int i = 1, j = 0; i < n; i++) {  // bit reversal
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			std::swap(re[i], re[j]);
			std::swap(im[i], im[j]);
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2 * PI / len, wr = cosf(ang), wi = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float cr = 1, ci = 0;
			for (int k = 0; k < len / 2; k++) {
				int a = i + k, b = a + len / 2;
				float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				float nr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = nr;
			}
		}
	}
}

void audio_task(void*) {
	i2s_config_t cfg = {};
	cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
	cfg.sample_rate = RATE;
	cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;  // INMP441: 24 bits in a 32-bit slot
	// both slots: the mic sends one (L/R pin), the other is silent; this
	// driver's ONLY_LEFT / ONLY_RIGHT are known to be swapped on the ESP32
	cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
	cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
	cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
	cfg.dma_buf_count = 4;
	cfg.dma_buf_len = CHUNK;
	i2s_pin_config_t pins = {};
	pins.mck_io_num = I2S_PIN_NO_CHANGE;
	pins.bck_io_num = PIN_SCK;
	pins.ws_io_num = PIN_WS;
	pins.data_out_num = I2S_PIN_NO_CHANGE;
	pins.data_in_num = PIN_SD;

	float* re = (float*)malloc(N * sizeof(float));
	float* im = (float*)malloc(N * sizeof(float));
	float* ring = (float*)malloc(N * sizeof(float));  // the last N samples, -1 .. 1
	float* mag = (float*)calloc(Audio::BINS, sizeof(float));  // smoothed magnitudes
	int32_t* raw = (int32_t*)malloc(CHUNK * 2 * sizeof(int32_t));
	bool ok = re && im && ring && mag && raw;
	if (ok && i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK)
		ok = false;
	else if (ok && i2s_set_pin(PORT, &pins) != ESP_OK) {
		i2s_driver_uninstall(PORT);
		ok = false;
	}
	if (!ok)
		Log.line(LogLevel::Error, "audio: not enough memory for the microphone");
	else
		Log.println("audio: microphone on");

	int fill = 0, since_fft = 0, slot = -1;  // slot: the one the mic talks on
	float dc = 0;
	while (ok && millis() - last_read < IDLE_MS) {
		size_t got = 0;
		if (i2s_read(PORT, raw, CHUNK * 2 * sizeof(int32_t), &got, pdMS_TO_TICKS(100)) != ESP_OK || !got)
			continue;
		int frames = got / (2 * sizeof(int32_t));
		if (slot < 0) {  // the first data: which slot carries it
			int64_t e[2] = {0, 0};
			for (int i = 0; i < frames; i++)
				for (int c = 0; c < 2; c++)
					e[c] += llabs((int64_t)(raw[2 * i + c] >> 8));
			if (e[0] || e[1])
				slot = e[1] > e[0] ? 1 : 0;
		}
		for (int i = 0; i < frames; i++) {
			float s = (raw[2 * i + (slot < 0 ? 0 : slot)] >> 8) / 8388608.0f;  // 24 bits → -1 .. 1
			dc += (s - dc) * 0.001f;  // the INMP441's DC offset, removed
			ring[fill] = s - dc;
			fill = (fill + 1) % N;
		}
		since_fft += frames;
		if (since_fft < N)
			continue;
		since_fft = 0;

		// Hann window, oldest sample first
		for (int i = 0; i < N; i++) {
			re[i] = ring[(fill + i) % N] * (0.5f - 0.5f * cosf(2 * PI * i / (N - 1)));
			im[i] = 0;
		}
		fft(re, im, N);
		uint8_t f[Audio::BINS], w[Audio::BINS];
		for (int k = 0; k < Audio::BINS; k++) {
			float m = sqrtf(re[k] * re[k] + im[k] * im[k]) / N;
			mag[k] = SMOOTHING * mag[k] + (1 - SMOOTHING) * m;
			float db = mag[k] > 0 ? 20 * log10f(mag[k]) : MIN_DB;
			f[k] = (uint8_t)constrain((db - MIN_DB) / (MAX_DB - MIN_DB) * 255, 0.0f, 255.0f);
			float s = ring[(fill + N - Audio::BINS + k) % N];  // the newest samples
			w[k] = (uint8_t)constrain(128 + s * 128, 0.0f, 255.0f);
		}
		portENTER_CRITICAL(&lock);
		memcpy(fft_out, f, sizeof(f));
		memcpy(wave_out, w, sizeof(w));
		portEXIT_CRITICAL(&lock);
	}

	if (ok) {
		i2s_driver_uninstall(PORT);
		Log.println("audio: microphone off");
	}
	free(re);
	free(im);
	free(ring);
	free(mag);
	free(raw);
	portENTER_CRITICAL(&lock);
	memset(fft_out, 0, sizeof(fft_out));
	memset(wave_out, 128, sizeof(wave_out));
	task = nullptr;
	portEXIT_CRITICAL(&lock);
	vTaskDelete(nullptr);
}

// a read: keeps the microphone on (starts it if needed)
void touch() {
	last_read = millis();
	portENTER_CRITICAL(&lock);
	bool start = task == nullptr;
	if (start)
		task = reinterpret_cast<TaskHandle_t>(1);  // being created
	portEXIT_CRITICAL(&lock);
	if (start && xTaskCreatePinnedToCore(audio_task, "AudioTask", 4096, nullptr, 2, &task, 0) != pdPASS) {
		task = nullptr;
		Log.line(LogLevel::Error, "audio: not enough memory to start");
	}
}

struct Init {
	Init() { memset(wave_out, 128, sizeof(wave_out)); }
} init;

}  // namespace

namespace Audio {

void fft(uint8_t out[BINS]) {
	touch();
	portENTER_CRITICAL(&lock);
	memcpy(out, fft_out, BINS);
	portEXIT_CRITICAL(&lock);
}

void wave(uint8_t out[BINS]) {
	touch();
	portENTER_CRITICAL(&lock);
	memcpy(out, wave_out, BINS);
	portEXIT_CRITICAL(&lock);
}

}  // namespace Audio
