#!/bin/sh
# Build the TJC video player (WSL): sh apps/tjcplay/build.sh -> apps/tjcplay/TJCPLAY.BIN
set -e
cd "$(dirname "$0")/../.."
OBJ=build_tjcplay sh sdk/build.sh apps/tjcplay/TJCPLAY.BIN apps/tjcplay/tjcplay.c apps/tjcplay/tjcdec.c
