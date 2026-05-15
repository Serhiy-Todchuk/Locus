# strip_em_dashes.ps1 -- one-shot cleanup of non-ASCII typographic chars.
# Delegates to the Python script; PowerShell wrapper provided for discoverability.
#
# Usage:
#   .\scripts\strip_em_dashes.ps1              # apply changes
#   .\scripts\strip_em_dashes.ps1 --dry-run    # preview only

param(
    [switch]$DryRun
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$pythonScript = Join-Path $scriptDir "strip_em_dashes.py"

if ($DryRun) {
    python $pythonScript --dry-run
} else {
    python $pythonScript
}
