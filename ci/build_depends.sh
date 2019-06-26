#!/usr/bin/env bash

# This script is executed inside the builder image

set -e

source ./ci/matrix.sh

unset CC; unset CXX
unset DISPLAY

mkdir -p $CACHE_DIR/depends
mkdir -p $CACHE_DIR/sdk-sources

if [ ! -e depends/built ]; then
	ln -s $CACHE_DIR/depends depends/built
fi
if [ ! -e depends/sdk-sources ]; then
	ln -s $CACHE_DIR/sdk-sources depends/sdk-sources
fi

mkdir -p depends/SDKs

if [ -n "$OSX_SDK" ]; then
  if [ ! -f depends/sdk-sources/MacOSX${OSX_SDK}.sdk.tar.gz ]; then
    curl --location --fail $SDK_URL/MacOSX${OSX_SDK}.sdk.tar.gz -o depends/sdk-sources/MacOSX${OSX_SDK}.sdk.tar.gz
  fi
  if [ -f depends/sdk-sources/MacOSX${OSX_SDK}.sdk.tar.gz ]; then
    tar -C depends/SDKs -xf depends/sdk-sources/MacOSX${OSX_SDK}.sdk.tar.gz
  fi
fi

make $MAKEJOBS -C depends HOST=$HOST $DEP_OPTS &> build-depends.log || (tail -n1000 build-depends.log && false)
