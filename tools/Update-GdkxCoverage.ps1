<#
.SYNOPSIS
    Regenerates docs/compatibility/gdkx-coverage.svg and the console-only API
    matrix in COMPATIBILITY.md from docs/compatibility/gdkx-status.json.

.PARAMETER Check
    Don't write anything; exit 1 if either output is out of date.
#>
param([switch]$Check)

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'Update-Compatibility.ps1') -Check:$Check `
    -StatusFile 'docs/compatibility/gdkx-status.json' `
    -ImageFile 'docs/compatibility/gdkx-coverage.svg' `
    -Title 'OpenEra console-only (GDKX) API coverage' `
    -Marker 'gdkx-matrix' `
    -Generator 'tools/Update-GdkxCoverage.ps1'
exit $LASTEXITCODE
