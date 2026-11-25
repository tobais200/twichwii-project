#!/usr/bin/env bash
# Inicia el servidor companion de TwitchWii en 0.0.0.0:8788 (pasale --mock, --port, --play-mode, etc.)
cd "$(dirname "$0")" && exec python3 twitchwii_server.py "$@"
