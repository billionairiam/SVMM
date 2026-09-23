#ifndef CONSOLE_H
#define CONSOLE_H

#include <pthread.h>
#include <stdbool.h>
#include <termios.h>

struct serial;

typedef void (*console_quit_fn)(void *opaque);

struct console {
    struct serial *serial;
    int in_fd;
    bool raw;
    bool started;
    struct termios saved;
    pthread_t thread;
    console_quit_fn quit;
    void *quit_opaque;
};

int console_start(struct console *console, struct serial *serial, int in_fd,
                  console_quit_fn quit, void *quit_opaque);
void console_stop(struct console *console);

#endif
