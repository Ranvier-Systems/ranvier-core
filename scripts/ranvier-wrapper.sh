#!/bin/sh
# Wrapper script to handle --help before Seastar initialization blocks

for arg in "$@"; do
    case "$arg" in
        -h|--help)
            cat << 'HELP'
Ranvier Core - Content-aware Layer 7+ Load Balancer for LLM Inference

USAGE:
    ranvier_server [OPTIONS]

DESCRIPTION:
    Ranvier routes LLM requests based on token prefixes rather than
    connection availability, reducing GPU cache thrashing by directing
    requests to backends that already hold relevant KV cache state.

OPTIONS:
    -h, --help              Print this help message and exit
    --config <PATH>         Path to configuration file (default: ranvier.yaml)
    --smp <N>               Number of CPU cores to use (Seastar option)
    --memory <SIZE>         Memory to allocate (e.g., 4G) (Seastar option)

SIGNALS:
    SIGHUP                  Reload configuration (hot-reload)
    SIGINT, SIGTERM         Graceful shutdown with connection draining

EXAMPLES:
    ranvier_server
        Start with default config file (ranvier.yaml)

    ranvier_server --config /etc/ranvier/config.yaml
        Start with custom config file

    ranvier_server --smp 4 --memory 8G
        Start with 4 CPU cores and 8GB memory

For more information, see: https://github.com/ranvier-systems/ranvier-core
HELP
            exit 0
            ;;
    esac
done

exec ./ranvier_server "$@"
