#!/bin/bash
set -e

GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

cd "$(dirname "$0")"

echo -e "${BLUE}Compilando tests...${NC}"
cmake -B build -DBUILD_TESTS=ON -DBUILD_SERVER=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build build --target unit_tests -j$(nproc)

echo -e "${BLUE}Ejecutando unit tests...${NC}"
./build/tests/unit_tests "$@"

echo -e "${GREEN}Tests completados${NC}"
