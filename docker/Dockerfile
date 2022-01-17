FROM debian:stretch
LABEL maintainer="Alterdot developers <dev@alterdot.org>"
LABEL description="Dockerised Alterdot, built from Travis"

RUN apt-get update && apt-get -y upgrade && apt-get clean && rm -fr /var/cache/apt/*

COPY bin/* /usr/bin/
