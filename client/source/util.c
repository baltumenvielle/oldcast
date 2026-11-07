#include "common.h"

#include <sys/stat.h>

bool g_linked;

void ensure_data_dir(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
}

bool wait_for_key(void)
{
    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START)
            return false;
        if (down)
            return true;
    }
    return false;
}
