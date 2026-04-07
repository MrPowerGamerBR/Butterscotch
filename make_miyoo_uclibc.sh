#!/bin/bash
# Build script for Miyoo platform using Docker (uclibc toolchain)

set -e

PROJECT_DIR="."
MAKE_CMD="cmake -B build_miyoo -DPLATFORM=miyoo -DCMAKE_TOOLCHAIN_FILE=cmake/miyoo.cmake -DCMAKE_BUILD_TYPE=Release . && cmake --build build_miyoo -j\$(nproc) && cmake --build build_miyoo --target ipk"

USER_GROUP=`id -gn`
CYAN="\033[1;36m"
NC="\033[0m"

echolog ()
{
	echo -e "\n${CYAN}[INFO]: ${1}${NC}\n"
}

echolog "Creating and starting container..."
CONTAINER_ID=`sudo docker run -v "$(pwd)/${PROJECT_DIR}":/src -itd docker.io/miyoocfw/toolchain-static-uclibc:latest`

echolog "Compiling in container..."
sudo docker exec -it -w "/src" "${CONTAINER_ID}" bash -c "${MAKE_CMD}"

echolog "Taking ownership of compiled files..."
sudo chown "${USER}":"${USER_GROUP}" "${PROJECT_DIR}"/*

echolog "Stopping container..."
sudo docker stop "${CONTAINER_ID}"
echolog "Deleting container..."
sudo docker rm "${CONTAINER_ID}"

echolog "Build complete!"
echo "Binary: ${PROJECT_DIR}/build_miyoo/butterscotch"
echo "IPK:    ${PROJECT_DIR}/build_miyoo/butterscotch.ipk"
