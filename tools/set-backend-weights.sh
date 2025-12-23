#!/usr/bin/env bash

# High-capacity primary (weight=200, priority=0)
curl -X POST "http://localhost:8080/admin/backends?id=1&ip=192.168.1.100&port=11434&weight=200&priority=0"

# Normal primary (weight=100, priority=0)  
curl -X POST "http://localhost:8080/admin/backends?id=2&ip=192.168.1.101&port=11434&weight=100&priority=0"

# Fallback backend (weight=100, priority=1)
curl -X POST "http://localhost:8080/admin/backends?id=3&ip=192.168.1.102&port=11434&weight=100&priority=1"

# With the above config, backend 1 gets 66% and backend 2 gets 33% of traffic. Backend 3 is only used if both primaries are down.
