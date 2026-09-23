.global _start
.set noreorder

_start:
    lui $t0, 0xb000       # Start scanning from 0xb0000000
    li  $t1, 0x41         # ASCII 'A'

sweep_loop:
    sb  $t1, 0($t0)       # Write test character
    
    # Simple delay loop so characters don't flood instantly
    li  $t3, 0x000fffff
delay:
    addiu $t3, $t3, -1
    bnez  $t3, delay
    nop

    addiu $t0, $t0, 0x1000 # Step forward through memory
    b     sweep_loop
    nop