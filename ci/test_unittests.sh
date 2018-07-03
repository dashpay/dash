#!/usr/bin/env bash

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

source $DIR/matrix.sh

$DOCKER_RUN_IN_BUILDER ./ci/test_unittests_in_builder.sh
