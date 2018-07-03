#!/usr/bin/env bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SRCDIR=$(realpath $DIR/..)

source $DIR/matrix.sh

HELPER_NAME="$SRC_VOLUME-helper"

# Make sure old src volume is not present anymore
docker volume rm $SRC_VOLUME

echo "creating volumes"
docker volume create $SRC_VOLUME
docker volume create $CACHE_VOLUME

docker run -d -u $UID --name $HELPER_NAME $DOCKER_RUN_VOLUME_ARGS busybox sleep 10000

echo "copying source to $SRC_VOLUME"
docker cp $HOST_SRC_DIR/. $HELPER_NAME:$SRC_DIR

docker exec -t $HELPER_NAME chown $UID:$UID $SRC_DIR /cache -R
docker rm -f $HELPER_NAME
