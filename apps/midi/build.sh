#!/bin/sh
# Build the MIDI player (WSL): sh apps/midi/build.sh -> apps/midi/MIDI.BIN
set -e
cd "$(dirname "$0")/../.."
OBJ=build_midi sh sdk/build.sh apps/midi/MIDI.BIN apps/midi/midi.c apps/midi/smf.c apps/midi/synth_opl.c apps/midi/synth_sf2.c sdk/opl.c
