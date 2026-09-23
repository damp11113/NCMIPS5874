__attribute__((naked)) void _start(void) {
    __asm__ volatile (
        "li $v0, 42\n\t"
        "jr $ra\n\t"
        "nop\n\t"
    );
}