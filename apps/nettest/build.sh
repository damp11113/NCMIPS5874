#!/bin/sh
# Build the network test app (WSL): sh apps/nettest/build.sh -> apps/nettest/NETTEST.BIN
set -e
cd "$(dirname "$0")/../.."
OBJ=build_nettest sh sdk/build.sh apps/nettest/NETTEST.BIN apps/nettest/nettest.c
