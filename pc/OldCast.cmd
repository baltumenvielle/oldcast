@echo off
rem Starts the OldCast host and its window, without a console window.
rem Double-click it, or create a desktop shortcut to it.
cd /d "%~dp0"
start "" ".venv\Scripts\pythonw.exe" -m oldcast
