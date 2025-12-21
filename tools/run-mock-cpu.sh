#!/usr/bin/env bash

#port=$@
port=9001
python3 tools/mock_gpu.py --port "${port}"
