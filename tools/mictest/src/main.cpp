// Microphone test: an INMP441 on I2S0, as wired on the Spectre Matrix driver
// (same pins as the firmware's src/Audio.cpp). Prints ten lines a second:
//
//   -38 dBFS |##########          | peak  440 Hz | ch0 -12034..11870 ch1 0..0 | SD high 48%
//
// and a diagnosis when something is off (SD stuck low / high, no clock…).
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

constexpr i2s_port_t PORT = I2S_NUM_0;
constexpr int PIN_SCK = 0, PIN_WS = 4, PIN_SD = 35;
constexpr int RATE = 16000;
constexpr int N = 512;  // samples per line batch for the FFT (32 ms)

int32_t raw[N * 2];  // stereo frames: the mic talks on one slot (L/R pin)
float re[N], im[N];

void fft(float* r, float* i, int n) {
	for (int a = 1, b = 0; a < n; a++) {
		int bit = n >> 1;
		for (; b & bit; bit >>= 1)
			b ^= bit;
		b ^= bit;
		if (a < b) {
			std::swap(r[a], r[b]);
			std::swap(i[a], i[b]);
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2 * PI / len, wr = cosf(ang), wi = sinf(ang);
		for (int s = 0; s < n; s += len) {
			float cr = 1, ci = 0;
			for (int k = 0; k < len / 2; k++) {
				int a = s + k, b = a + len / 2;
				float tr = r[b] * cr - i[b] * ci, ti = r[b] * ci + i[b] * cr;
				r[b] = r[a] - tr;
				i[b] = i[a] - ti;
				r[a] += tr;
				i[a] += ti;
				float nr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = nr;
			}
		}
	}
}

void setup() {
	Serial.begin(115200);
	delay(300);
	Serial.printf("\n\nINMP441 test: SCK GPIO %d, WS GPIO %d, SD GPIO %d, %d Hz, 32-bit stereo\n", PIN_SCK, PIN_WS, PIN_SD, RATE);
	i2s_config_t cfg = {};
	cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
	cfg.sample_rate = RATE;
	cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
	cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
	cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
	cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
	cfg.dma_buf_count = 8;
	cfg.dma_buf_len = 256;
	i2s_pin_config_t pins = {};
	pins.mck_io_num = I2S_PIN_NO_CHANGE;
	pins.bck_io_num = PIN_SCK;
	pins.ws_io_num = PIN_WS;
	pins.data_out_num = I2S_PIN_NO_CHANGE;
	pins.data_in_num = PIN_SD;
	esp_err_t e1 = i2s_driver_install(PORT, &cfg, 0, nullptr), e2 = i2s_set_pin(PORT, &pins);
	Serial.printf("I2S driver: %s, pins: %s\n", esp_err_to_name(e1), esp_err_to_name(e2));
	Serial.println("Talk, clap or play music near the mic (the INMP441 needs ~85 ms to start).\n");
}

void loop() {
	// one batch of N stereo frames
	size_t got = 0, total = 0;
	uint32_t t0 = millis();
	while (total < sizeof(raw)) {
		if (i2s_read(PORT, (uint8_t*)raw + total, sizeof(raw) - total, &got, pdMS_TO_TICKS(200)) != ESP_OK || !got)
			break;
		total += got;
	}
	int frames = total / 8;
	if (frames < N) {
		Serial.printf("no data from I2S (%d frames in %u ms): the driver is not clocking\n", frames, millis() - t0);
		delay(500);
		return;
	}
	// both slots: ranges, and which one carries the mic (L/R = GND: left)
	int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
	for (int i = 0; i < N; i++)
		for (int c = 0; c < 2; c++) {
			int32_t s = raw[2 * i + c] >> 8;  // 24-bit samples
			lo[c] = min(lo[c], s);
			hi[c] = max(hi[c], s);
		}
	int slot = (hi[1] - lo[1]) > (hi[0] - lo[0]) ? 1 : 0;

	// level (RMS, DC removed) and the loudest frequency
	double mean = 0;
	for (int i = 0; i < N; i++)
		mean += raw[2 * i + slot] >> 8;
	mean /= N;
	double sq = 0;
	for (int i = 0; i < N; i++) {
		float s = ((raw[2 * i + slot] >> 8) - mean) / 8388608.0;
		sq += s * s;
		re[i] = s * (0.5f - 0.5f * cosf(2 * PI * i / (N - 1)));
		im[i] = 0;
	}
	float rms = sqrt(sq / N);
	float db = rms > 0 ? 20 * log10f(rms) : -120;
	fft(re, im, N);
	int peak = 1;
	float best = 0;
	for (int k = 2; k < N / 2; k++) {  // above ~60 Hz
		float m = re[k] * re[k] + im[k] * im[k];
		if (m > best) {
			best = m;
			peak = k;
		}
	}

	// the SD pin itself: high some of the time when the mic talks
	int high = 0;
	for (int i = 0; i < 1000; i++)
		high += gpio_get_level((gpio_num_t)PIN_SD);

	char bar[31];
	int n = constrain((int)((db + 90) / 70 * 30), 0, 30);  // -90 .. -20 dBFS
	for (int i = 0; i < 30; i++)
		bar[i] = i < n ? '#' : ' ';
	bar[30] = 0;
	Serial.printf("%4.0f dBFS |%s| peak %5d Hz | ch0 %8d..%-8d ch1 %8d..%-8d | SD high %3d%%", db, bar,
	              (int)(peak * (float)RATE / N), lo[0], hi[0], lo[1], hi[1], high / 10);

	bool flat0 = lo[0] == hi[0], flat1 = lo[1] == hi[1];
	if (flat0 && flat1 && hi[0] == 0 && hi[1] == 0)
		Serial.print("  << SD stuck LOW: the mic sends nothing (power, EN, orientation, solder, or no clock at the mic)");
	else if (flat0 && flat1 && hi[0] == -1 && hi[1] == -1)
		Serial.print("  << SD stuck HIGH: shorted to 3.3 V?");
	else if (!flat0 && !flat1)
		Serial.print("  (both slots have data: L/R floating?)");
	Serial.println();
	delay(70);
}
