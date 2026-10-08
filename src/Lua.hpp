#include <NimBLEDevice.h>

namespace Lua {

  /**
   * Crate task for running lua scripts.
   */
  BaseType_t init();

  /**
   * Stop the running script (and drop a queued one), then wait until the Lua
   * task has released the arena. Returns false if it did not within 2 s
   * (e.g. a script stuck in a long delay()).
   */
  bool stop();

  /**
   * Run the supplied script.
   * If another script is running, it will stopped first.
   */
  void run_script(String script, String name = "script");

}