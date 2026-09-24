#!/bin/sh
# Disassemble a MIPS16 region of app_ram.bin: ./m16dis.sh <start-hex> <end-hex>
# (objdump only decodes MIPS16 when a mips16 function symbol covers the bytes)
S=$((($1) & ~15)); E=$(($2)); OFF=$((S - 0x80008000)); LEN=$((E - S))
T=${TMPDIR:-/tmp}/m16$$
cat > $T.S <<EOS
.set mips16
.text
.globl f
.ent f
f:
.insn
.incbin "app_ram.bin", $OFF, $LEN
.end f
EOS
printf 'SECTIONS { . = 0x%x; .text : { *(.text) } /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) } }\n' $S > $T.ld
mipsel-linux-gnu-as -mips32r2 -o $T.o $T.S
mipsel-linux-gnu-ld -T $T.ld -e f -o $T.elf $T.o
mipsel-linux-gnu-objdump -d $T.elf | tail -n +7
rm -f $T.S $T.o $T.elf $T.ld
