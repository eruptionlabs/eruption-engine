#!/bin/bash
# Fuzz dos parsers e da sessão de rede (src/net) com ASan + UBSan. Sem GPU nem motor.
#   tools/fuzz/run_net_fuzz.sh [segundos=600] [semente=1] [alvo=wire|session]
#   wire:    leitor de bits, um pacote por entrada
#   session: roteiros de um par com token válido contra o servidor e o cliente
# Com clang + libFuzzer no sistema dá para usar os mesmos alvos:
#   clang++ -std=c++17 -DERUPTION_LIBFUZZER -fsanitize=fuzzer,address,undefined \
#       -I src tools/fuzz/net_wire_fuzz.cpp src/net/Wire.cpp
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT"
SECONDS_TO_RUN="${1:-600}"
SEED="${2:-1}"
TARGET="${3:-wire}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
if [ "$TARGET" = wire ]; then
    g++ -std=c++17 $SAN -Wall -Wextra -Wpedantic -I src tools/fuzz/net_wire_fuzz.cpp src/net/Wire.cpp \
        -o "$TMP/fuzz"
else
    gcc $SAN -w -DNETCODE_RELEASE -I third_party/netcode/sodium -c third_party/netcode/netcode.c -o "$TMP/netcode.o"
    gcc $SAN -w -I third_party/netcode/sodium -c third_party/netcode/sodium/sodium.c -o "$TMP/sodium.o"
    gcc $SAN -w -DRELIABLE_RELEASE -c third_party/reliable/reliable.c -o "$TMP/reliable.o"
    gcc $SAN -w -c third_party/monocypher/monocypher.c -o "$TMP/monocypher.o"
    g++ -std=c++17 $SAN -Wall -Wextra -Wpedantic -I src -isystem third_party/netcode -isystem third_party/reliable \
        -isystem third_party/monocypher tools/fuzz/net_session_fuzz.cpp src/net/NetSession.cpp src/net/Channels.cpp \
        src/net/TokenGate.cpp src/net/NetValue.cpp src/net/Blake2b.cpp src/net/Wire.cpp src/net/NetcodeEndpoint.cpp \
        src/net/PacketLink.cpp src/net/LinkSim.cpp src/core/Logger.cpp \
        "$TMP/netcode.o" "$TMP/sodium.o" "$TMP/reliable.o" "$TMP/monocypher.o" -o "$TMP/fuzz"
fi
"$TMP/fuzz" "$SECONDS_TO_RUN" "$SEED"
