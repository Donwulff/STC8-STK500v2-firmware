/*
 * Pre-flash gate harness: exposes the protocol core on a pseudo-terminal so
 * a real avrdude can talk to it:
 *
 *   ./pty_harness [ptypath-file]     # prints "PTY /dev/pts/N"
 *   avrdude -c stk500pp -P /dev/pts/N -p t461a ...
 *
 * SIM_PROFILE / SIM_VERBOSE control the simulated target (see hal_mock.c).
 */

#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE    /* cfmakeraw */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include "stk500v2.h"

void mock_set_tx_fd(int fd);

int
main(int argc, char **argv)
{
    int master, slave;
    char *slavename;
    struct termios tio;
    uint8_t buf[512];
    ssize_t n, i;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        perror("openpt");
        return 1;
    }
    slavename = ptsname(master);
    if (!slavename) {
        perror("ptsname");
        return 1;
    }

    /* Hold our own slave fd open (keeps the master readable across avrdude
     * open/close cycles) and put the line in raw mode before any traffic. */
    slave = open(slavename, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        perror("open slave");
        return 1;
    }
    if (tcgetattr(slave, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(slave, TCSANOW, &tio);
    }

    printf("PTY %s\n", slavename);
    fflush(stdout);
    if (argc > 1) {
        FILE *f = fopen(argv[1], "w");
        if (f) {
            fprintf(f, "%s\n", slavename);
            fclose(f);
        }
    }

    mock_set_tx_fd(master);

    for (;;) {
        n = read(master, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            break;
        }
        if (n == 0)
            continue;
        for (i = 0; i < n; ++i)
            stk500v2_rx(buf[i]);
    }
    return 0;
}
