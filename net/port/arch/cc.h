/*
 * lwIP architecture header for the NC5874 boxes: MIPS 24KEc little endian,
 * sdk/libc, FreeRTOS (launcher system). See net/README.md.
 */
#ifndef NET_ARCH_CC_H
#define NET_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define BYTE_ORDER              LITTLE_ENDIAN
#define LWIP_TIMEVAL_PRIVATE    0           /* sdk/libc has struct timeval */
#define LWIP_PROVIDE_ERRNO      1           /* same numbers as sdk/libc */

#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
        printf("lwIP assert: %s (%s:%d)\n", (x), __FILE__, __LINE__); \
        for (;;) { } \
    } while (0)

/* CP0 Count mixed: only for ports, DNS ids, TCP ISNs */
static inline unsigned int net_arch_rand(void) {
    unsigned int c;

    __asm__ volatile("mfc0 %0, $9" : "=r" (c));
    return c * 2654435761u ^ (unsigned int) rand();
}
#define LWIP_RAND()             net_arch_rand()

#endif
