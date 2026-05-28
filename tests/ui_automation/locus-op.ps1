# locus-op.ps1 -- single-shot TCP client for the agentic-server JSON protocol.
#
# Goal: replace the ~7-line PowerShell preamble (TcpClient + StreamReader +
# UTF8 encoding + finally close) that every Bash tool call used to inline.
# See: tests/ui_automation/AGENTIC_TESTING.md for the full op catalog.
#
# Usage:
#   .\locus-op.ps1 -Json '{"op":"ping"}'
#   .\locus-op.ps1 -Json '{"op":"info"}' -Port 7878
#   .\locus-op.ps1 -Op "ping"                                    # shorthand
#   .\locus-op.ps1 -Op "submit_chat" -Args @{ text = "hi" }      # builds JSON
#   .\locus-op.ps1 -Op "read_chat"   -Args @{ max_chars = 4000 } | ConvertFrom-Json
#
# Defaults:
#   -Port  7878   (override or read from <output_dir>/agentic.port)
#   -Host  127.0.0.1
#
# Exit codes:
#   0  request sent + response returned (whether ok=true or ok=false)
#   2  connection refused / socket error (server probably not running)
#   3  bad argument combination
#
# The script writes ONE JSON line to stdout -- safe to pipe through
# ConvertFrom-Json. Errors go to stderr.

[CmdletBinding(DefaultParameterSetName = 'Json')]
param(
    # Raw JSON request line. Use this when you've already composed the request.
    [Parameter(ParameterSetName = 'Json', Position = 0)]
    [string]$Json,

    # Op name (without args) -- builds {"op":"<Op>"}.
    [Parameter(ParameterSetName = 'Op', Mandatory = $true)]
    [string]$Op,

    # Optional args hashtable, merged into the built JSON's "args" field.
    [Parameter(ParameterSetName = 'Op')]
    [hashtable]$Args = $null,

    # Optional client tag, echoed back in the response.
    [Parameter(ParameterSetName = 'Op')]
    [string]$Id = $null,

    [int]$Port = 7878,
    [string]$BindHost = '127.0.0.1',
    [int]$TimeoutMs = 600000
)

if ($PSCmdlet.ParameterSetName -eq 'Op') {
    $req = @{ op = $Op }
    if ($null -ne $Args) { $req['args'] = $Args }
    if ($null -ne $Id -and $Id -ne '') { $req['id'] = $Id }
    $Json = $req | ConvertTo-Json -Compress -Depth 12
}

if (-not $Json) {
    Write-Error "locus-op: provide -Json '<request>' or -Op '<name>' [-Args @{...}]"
    exit 3
}

try {
    $client = New-Object System.Net.Sockets.TcpClient
    $iar = $client.BeginConnect($BindHost, $Port, $null, $null)
    if (-not $iar.AsyncWaitHandle.WaitOne($TimeoutMs)) {
        $client.Close()
        Write-Error "locus-op: connect to ${BindHost}:${Port} timed out after ${TimeoutMs} ms"
        exit 2
    }
    $client.EndConnect($iar)

    $stream = $client.GetStream()
    $stream.ReadTimeout = $TimeoutMs

    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Json + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()

    $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
    $line = $reader.ReadLine()
    if ($null -eq $line) {
        Write-Error "locus-op: server closed connection without a response"
        exit 2
    }
    Write-Output $line
}
catch [System.Net.Sockets.SocketException] {
    Write-Error "locus-op: socket error talking to ${BindHost}:${Port}: $($_.Exception.Message)"
    exit 2
}
catch {
    Write-Error "locus-op: $($_.Exception.Message)"
    exit 2
}
finally {
    if ($client -and $client.Connected) { $client.Close() }
}
