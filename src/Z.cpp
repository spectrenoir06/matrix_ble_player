#include <Mapping.h>
#include <atomic>

#ifdef USE_SD
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#define filesystem SD
#endif
#ifdef USE_SPIFFS
#include "SPIFFS.h"
#define filesystem SPIFFS
#endif

extern VirtualMatrixPanel *virtualDisp;
extern void flip_matrix();

namespace {
  static TaskHandle_t task = NULL;
  static File current_file;
  static uint32_t next_frame_millis = 0;
  static int spectre_z_plz_stop = 1;
  static std::atomic<char*> next_z_file(nullptr);
  uint32_t fps;
  uint32_t wait;
  uint32_t nb;

  void zTask(void *parameter) {
    int t, i;
    for (;;) {
      vTaskDelay(1 / portTICK_PERIOD_MS);
      if (spectre_z_plz_stop) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        continue;
      }
      if (next_z_file.load() != nullptr) {
        char* fp = next_z_file.exchange(nullptr, std::memory_order_acq_rel);
        if (fp != nullptr) {
          if (current_file) { // close old gif file
            current_file.close();
          }
          current_file = filesystem.open(fp, FILE_READ);
          free(fp);
          if (current_file.size() > 0) {
            uint8_t head[5];
            current_file.read(head, 5);
            // TODO: 
            fps = (head[2] << 8) | head[1];
            nb = (head[4] << 8) | head[3];
            wait = 1000.0 / fps;
            if (wait > 2000)
              wait = 100;

            Serial.printf("  FPS: %02d; nb: %04d format: %d\n", fps, nb, head[0]);
            spectre_z_plz_stop = 0;
            next_frame_millis = 0;
            // clear both buffers
            virtualDisp->clearScreen();
            flip_matrix();
            virtualDisp->clearScreen();
          } else {
            // error
            spectre_z_plz_stop = 1;
            continue;
          }
        }
      }
      // next frame
      t = millis();
      if (t < next_frame_millis) continue;
      // TODO: display frame
      // current_file
      next_frame_millis = t + wait;
      flip_matrix();
    }
  }

}

namespace SpectreZ {

  void play(const char* filepath) {
    char* fp = strdup(filepath);
    char* old_fp = next_z_file.exchange(fp, std::memory_order_acq_rel);
    if (old_fp != NULL) {
      free(old_fp);
    }
    spectre_z_plz_stop = 0;
  }

  uint8_t init() {
    return xTaskCreate(
        zTask,   /* Task function. */
        "zTask", /* String with name of task. */
        8192 * 2,        /* Stack size in bytes. */
        NULL,            /* Parameter passed as input of the task */
        5,               /* Priority of the task. */
        &task);           /* Task handle. */
  }

  void stop() {
    spectre_z_plz_stop = 1;
  }

  bool isPlaying(const char* fp) {
    return current_file && strcmp(current_file.path(), fp) == 0;
  }

}