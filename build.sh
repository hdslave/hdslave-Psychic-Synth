#!/bin/sh
# Builds and tests Psychic in a Linux arm64 container: the image Melodee builds in (Debian bookworm, g++ 12,
# glibc 2.36, older than the MPC's 2.39). Pass make targets to run others, for example `./build.sh bench`.
set -eu
cd "$(dirname "$0")"
CONTEXT=${DOCKER_CONTEXT:-colima-melodee}
IMAGE=${PSYCHIC_IMAGE:-melodee-builder}
exec docker --context "$CONTEXT" run --rm --platform linux/arm64 -v "$PWD:/work" -w /work "$IMAGE" \
    sh -c "make -j4 ${*:-all test}"
