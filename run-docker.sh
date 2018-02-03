#!/bin/bash

docker volume create vbd_data
docker run -it -d -v vbd_data:/opt/run/vbd/data \
    -p9999:9999 -p127.0.0.1:9998:9998 vietlq/vbd-ubuntu
docker ps -a
