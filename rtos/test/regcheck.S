/*
 * int reg_check (uint32_t seed, uint32_t loops)
 * Fills every general register the port saves (except sp / gp / ra, and
 * k0 / k1) plus HI / LO with values derived from seed, spins, then
 * checks them. Returns the number of registers that changed: anything
 * but 0 means a context switch lost a register.
 */
        .set    noreorder
        .set    noat
        .text
        .globl  reg_check
        .ent    reg_check
reg_check:
        addiu   $sp, $sp, -48
        sw      $16, 0($sp)
        sw      $17, 4($sp)
        sw      $18, 8($sp)
        sw      $19, 12($sp)
        sw      $20, 16($sp)
        sw      $21, 20($sp)
        sw      $22, 24($sp)
        sw      $23, 28($sp)
        sw      $30, 32($sp)
        sw      $31, 36($sp)
        sw      $5, 40($sp)             /* loops */

        /* r = seed + n * 0x01010101 for registers n */
        li      $1, 0x01010101
        addu    $2, $4, $1
        addu    $3, $2, $1
        addu    $6, $3, $1
        addu    $7, $6, $1
        addu    $8, $7, $1
        addu    $9, $8, $1
        addu    $10, $9, $1
        addu    $11, $10, $1
        addu    $12, $11, $1
        addu    $13, $12, $1
        addu    $14, $13, $1
        addu    $15, $14, $1
        addu    $16, $15, $1
        addu    $17, $16, $1
        addu    $18, $17, $1
        addu    $19, $18, $1
        addu    $20, $19, $1
        addu    $21, $20, $1
        addu    $22, $21, $1
        addu    $23, $22, $1
        addu    $24, $23, $1
        addu    $25, $24, $1
        addu    $30, $25, $1
        mthi    $30
        mtlo    $25
        lw      $5, 40($sp)
1:      bnez    $5, 1b                  /* spin: get preempted many times */
        addiu   $5, $5, -1

        /* check: walk the chain again; v0 counts mismatches in $31 */
        move    $31, $0
        li      $1, 0x01010101
        addu    $5, $4, $1
        bne     $2, $5, 90f
        nop
2:      addu    $5, $5, $1
        bne     $3, $5, 91f
        nop
3:      addu    $5, $5, $1
        bne     $6, $5, 92f
        nop
4:      addu    $5, $5, $1
        bne     $7, $5, 93f
        nop
5:      addu    $5, $5, $1
        bne     $8, $5, 94f
        nop
6:      addu    $5, $5, $1
        bne     $9, $5, 95f
        nop
7:      addu    $5, $5, $1
        bne     $10, $5, 96f
        nop
8:      addu    $5, $5, $1
        bne     $11, $5, 97f
        nop
9:      addu    $5, $5, $1
        bne     $12, $5, 98f
        nop
10:     addu    $5, $5, $1
        bne     $13, $5, 99f
        nop
11:     addu    $5, $5, $1
        bne     $14, $5, 100f
        nop
12:     addu    $5, $5, $1
        bne     $15, $5, 101f
        nop
13:     addu    $5, $5, $1
        bne     $16, $5, 102f
        nop
14:     addu    $5, $5, $1
        bne     $17, $5, 103f
        nop
15:     addu    $5, $5, $1
        bne     $18, $5, 104f
        nop
16:     addu    $5, $5, $1
        bne     $19, $5, 105f
        nop
17:     addu    $5, $5, $1
        bne     $20, $5, 106f
        nop
18:     addu    $5, $5, $1
        bne     $21, $5, 107f
        nop
19:     addu    $5, $5, $1
        bne     $22, $5, 108f
        nop
20:     addu    $5, $5, $1
        bne     $23, $5, 109f
        nop
21:     addu    $5, $5, $1
        bne     $24, $5, 110f
        nop
22:     addu    $5, $5, $1
        bne     $25, $5, 111f
        nop
23:     addu    $5, $5, $1
        bne     $30, $5, 112f
        nop
24:     mfhi    $5
        bne     $30, $5, 113f
        nop
25:     mflo    $5
        bne     $25, $5, 114f
        nop
        b       200f
        nop

90:     b 2b
        addiu   $31, $31, 1
91:     b 3b
        addiu   $31, $31, 1
92:     b 4b
        addiu   $31, $31, 1
93:     b 5b
        addiu   $31, $31, 1
94:     b 6b
        addiu   $31, $31, 1
95:     b 7b
        addiu   $31, $31, 1
96:     b 8b
        addiu   $31, $31, 1
97:     b 9b
        addiu   $31, $31, 1
98:     b 10b
        addiu   $31, $31, 1
99:     b 11b
        addiu   $31, $31, 1
100:    b 12b
        addiu   $31, $31, 1
101:    b 13b
        addiu   $31, $31, 1
102:    b 14b
        addiu   $31, $31, 1
103:    b 15b
        addiu   $31, $31, 1
104:    b 16b
        addiu   $31, $31, 1
105:    b 17b
        addiu   $31, $31, 1
106:    b 18b
        addiu   $31, $31, 1
107:    b 19b
        addiu   $31, $31, 1
108:    b 20b
        addiu   $31, $31, 1
109:    b 21b
        addiu   $31, $31, 1
110:    b 22b
        addiu   $31, $31, 1
111:    b 23b
        addiu   $31, $31, 1
112:    b 24b
        addiu   $31, $31, 1
113:    b 25b
        addiu   $31, $31, 1
114:    addiu   $31, $31, 1

200:    move    $2, $31
        lw      $16, 0($sp)
        lw      $17, 4($sp)
        lw      $18, 8($sp)
        lw      $19, 12($sp)
        lw      $20, 16($sp)
        lw      $21, 20($sp)
        lw      $22, 24($sp)
        lw      $23, 28($sp)
        lw      $30, 32($sp)
        lw      $31, 36($sp)
        jr      $31
        addiu   $sp, $sp, 48
        .end    reg_check

/*
 * int dsp_check (uint32_t seed, uint32_t loops)
 * Same idea for the DSP state: accumulators 1-3 (hi / lo) and the pos /
 * scount fields of DSPControl. Returns the number of values that changed.
 */
        .set    dsp
        .globl  dsp_check
        .ent    dsp_check
dsp_check:
        li      $8, 0x11111111
        xor     $9, $4, $8
        mthi    $9, $ac1
        sll     $10, $8, 1
        xor     $9, $4, $10
        mtlo    $9, $ac1
        sll     $10, $8, 2
        xor     $9, $4, $10
        mthi    $9, $ac2
        addu    $10, $10, $8
        xor     $9, $4, $10
        mtlo    $9, $ac2
        sll     $10, $8, 3
        xor     $9, $4, $10
        mthi    $9, $ac3
        addu    $10, $10, $8
        xor     $9, $4, $10
        mtlo    $9, $ac3
        andi    $9, $4, 0x3f            /* pos */
        andi    $10, $4, 0x0fc0         /* scount (bits 12:7 come from seed bits 11:6) */
        sll     $10, $10, 1
        or      $9, $9, $10
        wrdsp   $9, 0x03
1:      bnez    $5, 1b
        addiu   $5, $5, -1

        move    $2, $0
        li      $8, 0x11111111
        xor     $9, $4, $8
        mfhi    $11, $ac1
        bne     $9, $11, 2f
        nop
        b       3f
        nop
2:      addiu   $2, $2, 1
3:      sll     $10, $8, 1
        xor     $9, $4, $10
        mflo    $11, $ac1
        beq     $9, $11, 4f
        nop
        addiu   $2, $2, 1
4:      sll     $10, $8, 2
        xor     $9, $4, $10
        mfhi    $11, $ac2
        beq     $9, $11, 5f
        nop
        addiu   $2, $2, 1
5:      addu    $10, $10, $8
        xor     $9, $4, $10
        mflo    $11, $ac2
        beq     $9, $11, 6f
        nop
        addiu   $2, $2, 1
6:      sll     $10, $8, 3
        xor     $9, $4, $10
        mfhi    $11, $ac3
        beq     $9, $11, 7f
        nop
        addiu   $2, $2, 1
7:      addu    $10, $10, $8
        xor     $9, $4, $10
        mflo    $11, $ac3
        beq     $9, $11, 8f
        nop
        addiu   $2, $2, 1
8:      andi    $9, $4, 0x3f
        andi    $10, $4, 0x0fc0
        sll     $10, $10, 1
        or      $9, $9, $10
        rddsp   $11, 0x03
        andi    $11, $11, 0x1fbf        /* pos (5:0) + scount (12:7) */
        beq     $9, $11, 9f
        nop
        addiu   $2, $2, 1
9:      jr      $31
        nop
        .end    dsp_check
