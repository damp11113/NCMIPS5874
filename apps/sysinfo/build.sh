#!/bin/sh
# Build the system info app (WSL): sh apps/sysinfo/build.sh -> apps/sysinfo/SYSINFO.BIN
set -e
cd "$(dirname "$0")/../.."
OBJ=build_sysinfo sh sdk/build.sh apps/sysinfo/SYSINFO.BIN apps/sysinfo/sysinfo.c
