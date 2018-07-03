#!/usr/bin/env bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SRCDIR=$(realpath $DIR/..)

source $DIR/matrix.sh

docker build \
  --pull \
  -t $BUILDER_IMAGE_NAME \
  --build-arg USER_ID=$UID --build-arg GROUP_ID=$UID \
  --build-arg DPKG_ADD_ARCH="$DPKG_ADD_ARCH" \
  --build-arg PACKAGES="$PACKAGES" \
  -f ci/Dockerfile.builder ci
