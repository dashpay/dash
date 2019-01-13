#!/usr/bin/env bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd $DIR/..

DOCKER_IMAGE=${DOCKER_IMAGE:-waggox/waggoxd-develop}
DOCKER_TAG=${DOCKER_TAG:-latest}

BUILD_DIR=${BUILD_DIR:-.}

rm docker/bin/*
mkdir docker/bin
cp $BUILD_DIR/src/waggoxd docker/bin/
cp $BUILD_DIR/src/waggox-cli docker/bin/
cp $BUILD_DIR/src/waggox-tx docker/bin/
strip docker/bin/waggoxd
strip docker/bin/waggox-cli
strip docker/bin/waggox-tx

docker build --pull -t $DOCKER_IMAGE:$DOCKER_TAG -f docker/Dockerfile docker
