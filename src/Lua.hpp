#include <NimBLEDevice.h>

namespace Lua {

  /**
   * Crate task for running lua scripts.
   */
  void init();

  /**
   * Kindly ask the lua interpreter to stop execution of the current script.
   * It is safe to call this function even if no script is currently running.
   */
  void stop();

  /**
   * Run the supplied script.
   * If another script is running, it will stopped first.
   */
  void run_script(String script);

  void loop();

}