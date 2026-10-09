#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <stdlib.h>

/* 不用 assert：-DNDEBUG 下它会连同副作用一起消失。 */
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,         \
                    __LINE__, #cond);                                      \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#endif
