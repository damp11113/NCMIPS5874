#!/bin/sh
# Build the BriMod app (WSL): sh apps/brimod/build.sh -> apps/brimod/BRIMOD.BIN
set -e
cd "$(dirname "$0")/../.."
OBJ=build_brimod sh sdk/build.sh apps/brimod/BRIMOD.BIN apps/brimod/brimod.c
