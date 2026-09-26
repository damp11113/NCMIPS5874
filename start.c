/*
 * Entry point for every program. U-Boot 'go' jumps to the first byte of
 * the binary, and link.ld puts .text.start first. Linked by buildc.sh,
 * so your program only needs a normal main ().
 *
 * .bss (globals/statics without an initial value) is not in the .bin,
 * so its RAM holds leftovers from earlier programs: clear it here, as C
 * requires those variables to start at zero.
 *
 * C++ programs: global constructors (.init_array) run after that, before
 * main (). Global destructors never run.
 */
int main(int argc, char *argv[]);

extern unsigned int __bss_start[], __bss_end[];
extern void(*__init_array_start[]) (void);
extern void(*__init_array_end[]) (void);

__attribute__((section(".text.start")))
int _start(int argc, char *argv[]) {
    volatile unsigned int *p;
    void(**ctor) (void);

    for (p = __bss_start; p < __bss_end; p++) {
        *p = 0;
    }
    for (ctor = __init_array_start; ctor < __init_array_end; ctor++) {
        (*ctor) ();
    }
    return main(argc, argv);
}
