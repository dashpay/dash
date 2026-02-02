#!/usr/bin/env bash

## Install multilib
apt update
apt install -y gcc-multilib

## Install Node.JS
curl -fsSL https://deb.nodesource.com/setup_16.x | sudo -E bash -
apt install -y nodejs

## Install build target
rustup target install aarch64-unknown-linux-musl

chmod 777 -R /root/.cargo
mkdir -p /github/workspace/target
chmod 777 -R /github/workspace/target

ARCH=arm64 LIBC=musl npm run build -- --release --target=aarch64-unknown-linux-musl