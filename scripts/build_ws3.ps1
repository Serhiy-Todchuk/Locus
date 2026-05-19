# Locus -- build WS3 (mixed personal-documents library) test workspace.
#
# Composes a small read-only corpus that exercises every WS3 extractor path:
#
#   rfcs/        10 IETF RFCs as PDF (PDFium extractor coverage)
#   samples/     one DOCX + one XLSX from Apache POI test-data (OOXML)
#   notes/       hand-authored Markdown notes that cross-reference the RFCs
#                (drives cross-document synthesis prompts)
#   LOCUS.md     read-only / no-edits policy hint
#
# Note (S5.N): we originally shipped each RFC twice (PDF + TXT) to test
# cross-format retrieval, but the TXT-form RFCs muddied the recall@K
# numbers without adding extractor coverage we don't already prove on
# the PDF side. Dropped TXTs; if you want plain-text RFC coverage, drop
# your own *.txt files into rfcs/.
#
# Footprint is small (~10-20 MB). All sources are public domain or
# Apache-2.0; nothing is redistributed by this script -- it just downloads
# at runtime, same model as `models/download.ps1`.
#
# Usage:
#   pwsh ./scripts/build_ws3.ps1            # default workspace path
#   pwsh ./scripts/build_ws3.ps1 -Force     # re-download + overwrite notes

[CmdletBinding()]
param(
    [string]$WorkspaceRoot = 'D:\Projects\LocusTestWorkspaces\WS3_Documents',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Format-Size([long]$bytes) {
    if ($bytes -ge 1GB) { return "{0:N2} GB" -f ($bytes / 1GB) }
    if ($bytes -ge 1MB) { return "{0:N1} MB" -f ($bytes / 1MB) }
    return "{0:N0} KB" -f ($bytes / 1KB)
}

function Download-If-Missing($Url, $Dest, $MinBytes) {
    if ((Test-Path $Dest) -and -not $Force) {
        $size = (Get-Item $Dest).Length
        if ($size -ge $MinBytes) {
            return $true
        }
        Write-Host "[warn] $(Split-Path -Leaf $Dest) too small ($(Format-Size $size)) - re-downloading"
        Remove-Item $Dest -Force
    }
    $tmp = "$Dest.partial"
    if (Test-Path $tmp) { Remove-Item $tmp -Force }

    $oldPref = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
    } catch {
        Write-Host "[fail] $Url"
        Write-Host "       $($_.Exception.Message)"
        if (Test-Path $tmp) { Remove-Item $tmp -Force }
        return $false
    } finally {
        $ProgressPreference = $oldPref
    }

    $size = (Get-Item $tmp).Length
    if ($size -lt $MinBytes) {
        Remove-Item $tmp -Force
        Write-Host "[fail] $(Split-Path -Leaf $Dest) suspiciously small ($(Format-Size $size)) - skipping"
        return $false
    }
    Move-Item -Force $tmp $Dest
    Write-Host "[ok  ] $(Split-Path -Leaf $Dest) ($(Format-Size $size))"
    return $true
}

# 1. Directories.
New-Item -ItemType Directory -Force -Path $WorkspaceRoot              | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $WorkspaceRoot 'rfcs')    | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $WorkspaceRoot 'samples') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $WorkspaceRoot 'notes')   | Out-Null

Write-Host "Locus WS3 builder"
Write-Host "  Workspace : $WorkspaceRoot"
Write-Host ""

# 2. IETF RFCs (public domain). PDF only; see header note re: the TXT
#    drop in S5.N.
$Rfcs = @(
    @{ N = 768;  Title = 'UDP'           },
    @{ N = 791;  Title = 'IP'            },
    @{ N = 793;  Title = 'TCP'           },
    @{ N = 826;  Title = 'ARP'           },
    @{ N = 1035; Title = 'DNS'           },
    @{ N = 8259; Title = 'JSON'          },
    @{ N = 8446; Title = 'TLS 1.3'       },
    @{ N = 9112; Title = 'HTTP/1.1'      },
    @{ N = 7540; Title = 'HTTP/2'        },
    @{ N = 9114; Title = 'HTTP/3 (QUIC)' }
)

Write-Host "[step] Downloading RFCs ..."
$rfcsOk = 0
foreach ($r in $Rfcs) {
    $n = $r.N
    $pdfDest = Join-Path $WorkspaceRoot "rfcs\rfc$n.pdf"
    # Newer RFCs (9000+) have native PDF at /rfc/rfcN.pdf; older ones only
    # have the auto-rendered PDF at /pdfrfc/rfcN.txt.pdf. Try the modern
    # path first, fall back to the rendered path.
    # 2KB floor: RFC 768 (UDP) is only ~4KB; a 10KB threshold would have
    # spuriously rejected it.
    $okPdf = Download-If-Missing "https://www.rfc-editor.org/rfc/rfc$n.pdf" $pdfDest (2KB)
    if (-not $okPdf) {
        $okPdf = Download-If-Missing "https://www.rfc-editor.org/pdfrfc/rfc$n.txt.pdf" $pdfDest (2KB)
    }
    if ($okPdf) { $rfcsOk++ }
}
Write-Host "[done] RFCs : $rfcsOk / $($Rfcs.Count) complete"
Write-Host ""

# 3. Apache POI sample DOCX + XLSX. License: Apache-2.0.
Write-Host "[step] Downloading OOXML samples (Apache POI test-data) ..."
$docxOk = Download-If-Missing `
    'https://raw.githubusercontent.com/apache/poi/trunk/test-data/document/SampleDoc.docx' `
    (Join-Path $WorkspaceRoot 'samples\SampleDoc.docx') (2KB)
$xlsxOk = Download-If-Missing `
    'https://raw.githubusercontent.com/apache/poi/trunk/test-data/spreadsheet/SampleSS.xlsx' `
    (Join-Path $WorkspaceRoot 'samples\SampleSS.xlsx') (2KB)
if (-not $docxOk) {
    Write-Host "[hint] DOCX download failed -- drop your own .docx into samples/ to exercise the DOCX extractor."
}
if (-not $xlsxOk) {
    Write-Host "[hint] XLSX download failed -- drop your own .xlsx into samples/ to exercise the XLSX extractor."
}
Write-Host ""

# 4. Hand-authored Markdown notes that cross-reference the RFCs. These drive
#    the "cross-document synthesis" + "find the source" acceptance prompts.
$Notes = @{
    'networking.md' = @'
# Networking Stack -- Personal Notes

The TCP/IP stack is built bottom-up from a small number of building blocks.

## Layer 2 -- Link

Address Resolution Protocol (ARP) maps IP addresses to MAC addresses on a
local segment. The protocol is spelled out in RFC 826 (1982) and the wire
format hasn't meaningfully changed since.

## Layer 3 -- Network

Internet Protocol (IP) provides best-effort datagram delivery. RFC 791
(1981) defines IPv4. There is no built-in reliability, ordering, or
congestion control here -- those concerns live one layer up.

## Layer 4 -- Transport

Two flavours, same address space, different guarantees:

- **TCP** (RFC 793) -- connection-oriented, reliable byte stream. Three-way
  handshake, sliding window, retransmissions. Pay the latency, get the
  reliability.
- **UDP** (RFC 768) -- connectionless datagrams. Eight-byte header, no
  state, no guarantees. The right choice when an application can do its own
  retransmission policy or when loss is preferable to delay (audio, video,
  DNS, gaming).

## Layer 7 -- Naming

Domain Name System (DNS, RFC 1035) gives humans names instead of dotted
quads. Resolution is a hierarchical lookup; the wire format is its own
small binary protocol.
'@

    'web-protocols.md' = @'
# Web Protocols -- Evolution Notes

HTTP has gone through three major wire formats while keeping the same
request/response semantics.

## HTTP/1.1

The text-based protocol everyone knows. The current spec is RFC 9112
(2022), which obsoletes the older RFC 2616. Pipelining was added,
proved fragile, and is effectively unused in practice.

## HTTP/2

Binary framing over a single TCP connection, defined in RFC 7540 (2015).
Multiplexes requests, header compression (HPACK), server push (now
deprecated). Solves head-of-line blocking at the HTTP layer but inherits
TCP's head-of-line blocking at the transport layer.

## HTTP/3

The same HTTP semantics again, but over QUIC instead of TCP. RFC 9114
(2022). QUIC's per-stream loss recovery finally eliminates the TCP
head-of-line blocking problem, at the cost of running over UDP and
re-implementing everything TCP gave you (handshake, congestion control,
retransmission) inside the QUIC layer.

## Security

All three modern HTTP versions are typically deployed over TLS 1.3
(RFC 8446) -- the older TLS versions are deprecated in practice.
'@

    'data-formats.md' = @'
# Data Format Notes

## JSON

JavaScript Object Notation is specified twice: the ECMA-404 standard and
RFC 8259, which is the IETF reference. The two agree on syntax; RFC 8259
adds interoperability guidance (numbers should fit in IEEE 754 double,
duplicate keys are unspecified behaviour, ordering is unspecified).

Notable not-in-the-spec items:
- Comments. Anything calling itself "JSON with comments" is non-standard.
- Trailing commas. Not allowed. Some parsers accept them anyway.
- NaN, Infinity. Not representable.
- Date / time. JSON has no native type; ISO 8601 strings are convention.

## DNS Records (quick reference)

| Type  | Purpose                                  |
|-------|------------------------------------------|
| A     | IPv4 address for a name                  |
| AAAA  | IPv6 address                             |
| CNAME | Alias to another name                    |
| MX    | Mail exchange                            |
| TXT   | Free-form text; SPF / DKIM ride on this  |
| NS    | Delegation -- authoritative name servers |

Wire format and full semantics in RFC 1035.
'@

    'security.md' = @'
# Security -- Quick Reference

## TLS 1.3

RFC 8446 (2018). Major changes from 1.2: one-RTT handshake (zero-RTT
for resumption), removed support for static RSA key exchange (forward
secrecy mandatory), removed the AEAD-only cipher restriction (everything
is AEAD now), simplified the handshake state machine substantially.

The 0-RTT mode trades some security for latency -- replay protection is
the application's problem. Treat 0-RTT data as if it could be replayed.

## What's missing from this folder

There are no RFCs here for IPsec, SSH, OAuth, or OIDC. If a question
needs those, say so plainly -- the model should not fabricate coverage
this corpus does not have.
'@

    'reading-list.md' = @'
# Reading List

Things I want to revisit:

- The QUIC congestion-control story -- BBR vs Cubic on lossy paths.
- HPACK static table vs dynamic table tradeoffs (HTTP/2).
- Why DNS over HTTPS resolves the privacy story but breaks the operator
  story.
- Whether anyone still uses TCP keepalives in 2026.
- TLS 1.3 0-RTT replay window in real deployments.

(These are pointers, not answers. The corpus has the building blocks but
not the synthesis.)
'@
}

Write-Host "[step] Writing Markdown notes ..."
foreach ($name in $Notes.Keys) {
    $path = Join-Path $WorkspaceRoot "notes\$name"
    if ((Test-Path $path) -and -not $Force) {
        Write-Host "[skip] notes\$name (use -Force to overwrite)"
    } else {
        Set-Content -Path $path -Value $Notes[$name] -Encoding utf8
        Write-Host "[ok  ] notes\$name"
    }
}
Write-Host ""

# 5. LOCUS.md (read-only policy + hints).
$LocusMd = @'
This is a personal documents folder. All files are read-only.

Never propose creating, editing, or deleting any file in this workspace.
If the user asks for a change, explain what the change would be in prose
and stop -- do not call edit_file / write_file / delete_file.

Workspace layout:
- `rfcs/`    -- IETF RFCs as PDF.
- `samples/` -- a sample DOCX and XLSX so format coverage is exercised.
- `notes/`   -- short hand-authored Markdown notes that reference the RFCs.

When a question can be answered from one document, cite the file path.
When it requires synthesising across files, name each file you drew from.
If the answer is not present in this corpus, say so plainly rather than
inventing coverage -- this folder is intentionally small.

This workspace has no code. The `code-aware search` capability bucket
should stay off (Settings -> Capabilities).
'@
$LocusMdPath = Join-Path $WorkspaceRoot 'LOCUS.md'
if ((Test-Path $LocusMdPath) -and -not $Force) {
    Write-Host "[skip] LOCUS.md already present"
} else {
    Set-Content -Path $LocusMdPath -Value $LocusMd -Encoding utf8
    Write-Host "[ok  ] LOCUS.md"
}

# 6. Pre-seed .locus/config.json with index.max_file_size_kb=4096.
#    The Locus default is 1024 KB, which silently rejects RFC 9114 (1.18 MB)
#    and any other large PDF. Without this, HTTP/3 retrieval drops out of
#    the WS3 acceptance set. The override goes in early so first-open
#    Locus reads it on initial index. If config.json already exists (mid-
#    test rebuilds), we patch only the one key and leave everything else.
$LocusDir = Join-Path $WorkspaceRoot '.locus'
$CfgPath  = Join-Path $LocusDir 'config.json'
New-Item -ItemType Directory -Force -Path $LocusDir | Out-Null

$patchPy = @'
import json, sys
path = sys.argv[1]
target_kb = 4096
try:
    with open(path, encoding="utf-8") as f:
        cfg = json.load(f)
except FileNotFoundError:
    cfg = {}
cfg.setdefault("index", {})["max_file_size_kb"] = target_kb
with open(path, "w", encoding="utf-8") as f:
    json.dump(cfg, f, indent=2)
print(f"[ok  ] config.json: index.max_file_size_kb={target_kb}")
'@
$tmpPatch = Join-Path $env:TEMP "locus_ws3_patch_cfg.py"
Set-Content -Path $tmpPatch -Value $patchPy -Encoding utf8
& python $tmpPatch $CfgPath
Remove-Item -Force $tmpPatch

Write-Host ""
Write-Host "Done. WS3 ready at: $WorkspaceRoot"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Launch locus_gui.exe `"$WorkspaceRoot`""
Write-Host "  2. Accept Workspace Capabilities defaults (code_aware_search off)"
Write-Host "  3. Wait for the first index + embedding queue to drain"
Write-Host "  4. Try a prompt: \"Which RFC defines HTTP/3?\""
