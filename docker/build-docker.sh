#!/usr/bin/env bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd $DIR/..

DOCKER_IMAGE=${DOCKER_IMAGE:-cadexproject/cadexd-develop}
DOCKER_TAG=${DOCKER_TAG:-latest}

BUILD_DIR=${BUILD_DIR:-.}

rm docker/bin/*
mkdir docker/bin
cp $BUILD_DIR/src/cadexd docker/bin/
cp $BUILD_DIR/src/cadex-cli docker/bin/
cp $BUILD_DIR/src/cadex-tx docker/bin/
strip docker/bin/cadexd
strip docker/bin/cadex-cli
strip docker/bin/cadex-tx

docker build --pull -t $DOCKER_IMAGE:$DOCKER_TAG -f docker/Dockerfile docker
