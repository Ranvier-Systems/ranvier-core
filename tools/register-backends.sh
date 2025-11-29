#!/usr/bin/env bash

curl -X POST "http://localhost:8080/admin/backends?id=91&port=9001"
echo ''

curl -X POST "http://localhost:8080/admin/backends?id=92&port=9002"
echo ''

curl -X POST "http://localhost:8080/admin/backends?id=93&port=9003"
echo ''
