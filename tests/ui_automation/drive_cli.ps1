# drive_cli.ps1 -- feed one (possibly multi-line) prompt to the locus CLI REPL,
# let the agent run the full turn to completion, then /quit. Captures all
# stdout+stderr to an output log. Uses bracketed-paste markers so a multi-line
# prompt is submitted as ONE message.
#
# Usage:
#   drive_cli.ps1 -PromptFile <path> -Workspace <dir> -OutFile <log> [-Model NAME] [-TimeoutSec N]
param(
    [Parameter(Mandatory=$true)][string]$PromptFile,
    [Parameter(Mandatory=$true)][string]$Workspace,
    [Parameter(Mandatory=$true)][string]$OutFile,
    [string]$Exe = "D:\Personal\Locus\build\release\Release\locus.exe",
    [string]$Model = "",
    [int]$TimeoutSec = 3600
)

$ESC = [char]27
$promptText = Get-Content -Raw -Path $PromptFile
# Build the stdin: bracketed-paste-wrapped prompt + newline to submit, then /quit.
$stdin = "$ESC[200~$promptText$ESC[201~`n/quit`n"

$tmpIn = [System.IO.Path]::GetTempFileName()
[System.IO.File]::WriteAllText($tmpIn, $stdin, (New-Object System.Text.UTF8Encoding($false)))

$args = @($Workspace)
if ($Model -ne "") { $args += @("--model", $Model) }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = ($args -join " ")
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi

$sbOut = New-Object System.Text.StringBuilder
$sbErr = New-Object System.Text.StringBuilder
$outEvent = Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
    if ($null -ne $Event.SourceEventArgs.Data) { [void]$Event.MessageData.AppendLine($Event.SourceEventArgs.Data) }
} -MessageData $sbOut
$errEvent = Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
    if ($null -ne $Event.SourceEventArgs.Data) { [void]$Event.MessageData.AppendLine($Event.SourceEventArgs.Data) }
} -MessageData $sbErr

[void]$proc.Start()
$proc.BeginOutputReadLine()
$proc.BeginErrorReadLine()

# Feed the whole stdin script, then close stdin so the REPL sees EOF after /quit.
$proc.StandardInput.Write([System.IO.File]::ReadAllText($tmpIn))
$proc.StandardInput.Close()

$done = $proc.WaitForExit($TimeoutSec * 1000)
if (-not $done) {
    "TIMEOUT after $TimeoutSec s -- killing locus.exe" | Out-Host
    try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
}
Start-Sleep -Milliseconds 300
Unregister-Event -SourceIdentifier $outEvent.Name -ErrorAction SilentlyContinue
Unregister-Event -SourceIdentifier $errEvent.Name -ErrorAction SilentlyContinue

$combined = "===== STDOUT =====`n" + $sbOut.ToString() + "`n===== STDERR =====`n" + $sbErr.ToString()
[System.IO.File]::WriteAllText($OutFile, $combined, (New-Object System.Text.UTF8Encoding($false)))
Remove-Item $tmpIn -ErrorAction SilentlyContinue

if ($done) { "EXITED code=$($proc.ExitCode)" | Out-Host } else { "KILLED (timeout)" | Out-Host }
