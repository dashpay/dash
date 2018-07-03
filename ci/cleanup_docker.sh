#!/usr/bin/env bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SRCDIR=$(realpath $DIR/..)

source $DIR/matrix.sh

# we only remove the SRC volume and leave the cache volume for future runs
docker volume rm $SRC_VOLUME

# Output some information about docker stuff
docker system df -v

# Exit 0 as previous docker calls might have failed
exit 0
