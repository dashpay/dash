FROM debian:stretch
LABEL maintainer="Cadex Developers <dev@cadex.xyz>"
LABEL description="Dockerised Cadex, built from Travis"

RUN apt-get update && apt-get -y upgrade && apt-get clean && rm -fr /var/cache/apt/*

COPY bin/* /usr/bin/
