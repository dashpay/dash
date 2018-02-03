#!/bin/bash

apt-get update -yqq

apt-get install -yqq libboost-dev libboost-chrono-dev \
    libboost-filesystem-dev libboost-program-options-dev \
    libboost-system-dev libboost-test-dev libboost-thread-dev \
    wget nano bsdmainutils curl libevent-dev automake \
    libdb++-dev build-essential libtool autotools-dev \
    autoconf pkg-config libssl-dev libboost-all-dev \
    libminiupnpc-dev python-virtualenv git \
    software-properties-common python-software-properties \
    g++ virtualenv dh-autoreconf sudo

add-apt-repository ppa:bitcoin/bitcoin

apt-get update -yqq

apt-get install libdb4.8-dev libdb4.8++-dev

apt-get install -yqq libqt5core5a libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler

apt-get upgrade -yqq
