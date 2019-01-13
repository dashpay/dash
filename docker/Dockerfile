FROM debian:stretch
LABEL maintainer="Waggox Developers <dev@waggox>"
LABEL description="Dockerised WaggoxCore, built from Travis"

RUN apt-get update && apt-get -y upgrade && apt-get clean && rm -fr /var/cache/apt/*

COPY bin/* /usr/bin/
