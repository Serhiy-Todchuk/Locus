#!/usr/bin/env bash
# Thin wrapper around locus-op.ps1 with ExecutionPolicy Bypass.
# Usage: op.sh '<raw json>'
exec powershell -NoProfile -ExecutionPolicy Bypass -File "d:/Personal/Locus/tests/ui_automation/locus-op.ps1" -Json "$1"
