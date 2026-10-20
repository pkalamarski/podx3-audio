// Step 0: open the POD X3 next to the running control app, claim interface 0,
// select alt 1, print descriptors, hold for a while, then restore alt 0.
#include "../usb/podx3_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int hold = argc > 1 ? atoi(argv[1]) : 5;
    podx3_t *dev;
    if (podx3_open(&dev, true)) return 1;
    printf("interface 0 alt 1 selected; holding %d s (check ioreg now)\n", hold);
    fflush(stdout);
    const char *cmd = getenv("PROBE_CMD"); // e.g. an ioreg query run while alt 1 is held
    if (cmd) system(cmd);
    sleep((unsigned)hold);
    podx3_close(dev);
    printf("released interface 0\n");
    return 0;
}
