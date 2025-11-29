#!/usr/bin/env bash

data=$@
curl -X POST -d "${data}" http://localhost:8080/v1/chat/completions
echo ''
