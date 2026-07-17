param(
    [Parameter(Mandatory=$true)][string]$File,
    [int]$Port = 7878,
    [int]$TimeoutMs = 20000
)
$text = Get-Content -Raw -Path $File
$req = @{ op = 'submit_chat'; args = @{ text = $text } } | ConvertTo-Json -Compress -Depth 12
& "$PSScriptRoot\locus-op.ps1" -Json $req -Port $Port -TimeoutMs $TimeoutMs
