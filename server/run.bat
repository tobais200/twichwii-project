@echo off
REM Inicia el servidor companion de TwitchWii en 0.0.0.0:8788 (Windows)
cd /d "%~dp0"
python twitchwii_server.py %*
pause
