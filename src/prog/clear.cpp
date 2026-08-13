#include "kernel/screen.h"
#include "user/prog.h"

BRAAM_PROGRAM(prog_clear, "clear", "blank the screen") {
    screen_clear();
    co_return 0;
}
