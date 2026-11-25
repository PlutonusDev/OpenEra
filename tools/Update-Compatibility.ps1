<#
.SYNOPSIS
    Regenerates docs/compatibility/api-coverage.svg and the API matrix in
    COMPATIBILITY.md from docs/compatibility/api-status.json.

.PARAMETER Check
    Don't write anything; exit 1 if either output is out of date.
#>
param([switch]$Check)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$dataPath = Join-Path $root 'docs/compatibility/api-status.json'
$svgPath = Join-Path $root 'docs/compatibility/api-coverage.svg'
$markdownPath = Join-Path $root 'COMPATIBILITY.md'
$invariant = [Globalization.CultureInfo]::InvariantCulture
$utf8 = New-Object System.Text.UTF8Encoding($false)

# Status order is also the order cells are drawn in.
$statusOrder = @('implemented', 'partial', 'stubbed', 'missing')
$statusLabels = @{ implemented = 'Implemented'; partial = 'Partial'; stubbed = 'Stubbed'; missing = 'Missing' }
$statusEmoji = @{
    implemented = [char]::ConvertFromUtf32(0x2705)   # white heavy check mark
    partial     = [char]::ConvertFromUtf32(0x1F7E1)  # yellow circle
    stubbed     = [char]::ConvertFromUtf32(0x1F7E3)  # purple circle
    missing     = [char]::ConvertFromUtf32(0x2B1C)   # white large square
}

function Format-Percent([int]$Part, [int]$Whole)
{
    if ($Whole -eq 0) { return '0.0%' }
    return ([double]$Part * 100 / $Whole).ToString('0.0', $invariant) + '%'
}

function ConvertTo-Xml([string]$Text)
{
    return $Text.Replace('&', '&amp;').Replace('<', '&lt;').Replace('>', '&gt;').Replace('"', '&quot;')
}

# Rough text width for the SVG fonts; only used to size cards.
function Measure-Text([string]$Text, [double]$Size)
{
    return [int][math]::Ceiling($Text.Length * $Size * 0.6)
}

function New-Counts
{
    $counts = [ordered]@{ total = 0 }
    foreach ($status in $statusOrder) { $counts[$status] = 0 }
    return $counts
}

# ---------------------------------------------------------------- Load data
$data = Get-Content -Raw -Encoding UTF8 $dataPath | ConvertFrom-Json
$categories = New-Object System.Collections.Generic.List[object]
$grand = New-Counts
foreach ($category in $data.categories)
{
    $categoryCounts = New-Counts
    $modules = New-Object System.Collections.Generic.List[object]
    foreach ($module in $category.modules)
    {
        $functions = New-Object System.Collections.Generic.List[object]
        $counts = New-Counts
        foreach ($property in $module.functions.PSObject.Properties)
        {
            $status = [string]$property.Value
            if ($statusOrder -notcontains $status)
            {
                throw "Unknown status '$status' for $($module.name)/$($property.Name) in $dataPath"
            }
            $functions.Add([pscustomobject]@{ Name = $property.Name; Status = $status })
            $counts[$status]++
            $counts.total++
        }
        foreach ($key in @($counts.Keys)) { $categoryCounts[$key] += $counts[$key] }
        $modules.Add([pscustomobject]@{ Name = [string]$module.name; Functions = $functions; Counts = $counts })
    }
    foreach ($key in @($categoryCounts.Keys)) { $grand[$key] += $categoryCounts[$key] }
    $categories.Add([pscustomobject]@{ Name = [string]$category.name; Modules = $modules; Counts = $categoryCounts })
}

# ---------------------------------------------------------------- SVG
$width = 1000
$margin = 28
$cell = 12
$cellGap = 3
$cardPad = 12
$cardTitle = 22
$cardGap = 12
$usable = $width - (2 * $margin)
$font = "system-ui,-apple-system,'Segoe UI',Helvetica,Arial,sans-serif"

$body = New-Object System.Text.StringBuilder
function Add-Line([string]$Line) { [void]$body.Append($Line).Append("`n") }

function Add-StatusBar([int]$X, [int]$Y, [int]$BarWidth, [int]$Height, $Counts)
{
    Add-Line ('  <rect class="track" x="{0}" y="{1}" width="{2}" height="{3}" rx="{4}"/>' -f $X, $Y, $BarWidth, $Height, [int]($Height / 2))
    if ($Counts.total -eq 0) { return }
    $offset = 0
    foreach ($status in $statusOrder)
    {
        if ($status -eq 'missing' -or $Counts[$status] -eq 0) { continue }
        $segment = [int][math]::Round($BarWidth * $Counts[$status] / $Counts.total)
        if ($segment -lt 2) { $segment = 2 }
        Add-Line ('  <rect class="{0}" x="{1}" y="{2}" width="{3}" height="{4}" rx="{5}"/>' -f $status, ($X + $offset), $Y, $segment, $Height, [int]($Height / 2))
        $offset += $segment
    }
}

# Header and legend.
$y = $margin
Add-Line ('  <text class="title" x="{0}" y="{1}">OpenEra API coverage</text>' -f $margin, ($y + 20))
$summary = '{0} of {1} tracked functions implemented ({2})' -f $grand.implemented, $grand.total, (Format-Percent $grand.implemented $grand.total)
Add-Line ('  <text class="meta" x="{0}" y="{1}">{2}</text>' -f $margin, ($y + 42), (ConvertTo-Xml $summary))

$legendX = $width - $margin
for ($i = $statusOrder.Count - 1; $i -ge 0; $i--)
{
    $status = $statusOrder[$i]
    $label = '{0} {1}' -f $statusLabels[$status], $grand[$status]
    $legendX -= (Measure-Text $label 12) + 18
    Add-Line ('  <rect class="{0}" x="{1}" y="{2}" width="12" height="12" rx="3"/>' -f $status, $legendX, ($y + 9))
    Add-Line ('  <text class="meta" x="{0}" y="{1}">{2}</text>' -f ($legendX + 17), ($y + 20), $label)
    $legendX -= 14
}
$y += 60
Add-StatusBar $margin $y $usable 8 $grand
$y += 32

foreach ($category in $categories)
{
    Add-Line ('  <text class="h2" x="{0}" y="{1}">{2}</text>' -f $margin, ($y + 14), (ConvertTo-Xml $category.Name))
    $meta = '{0}/{1} implemented ({2})' -f $category.Counts.implemented, $category.Counts.total, (Format-Percent $category.Counts.implemented $category.Counts.total)
    Add-Line ('  <text class="meta" x="{0}" y="{1}" text-anchor="end">{2}</text>' -f ($width - $margin), ($y + 14), $meta)
    $y += 24
    Add-StatusBar $margin $y $usable 4 $category.Counts
    $y += 14

    # Size each card at its natural width, largest modules first.
    $cards = foreach ($module in $category.Modules)
    {
        $count = $module.Counts.total
        $countText = '{0}/{1}' -f $module.Counts.implemented, $count
        $minInner = (Measure-Text $module.Name 12) + 10 + (Measure-Text $countText 11)
        $columns = [math]::Max(4, [int][math]::Ceiling([math]::Sqrt($count * 2.2)))
        $columns = [math]::Min($columns, [math]::Max($count, 1))
        while ($columns -lt $count -and ($columns * ($cell + $cellGap) - $cellGap) -lt $minInner) { $columns++ }
        $inner = [math]::Max($columns * ($cell + $cellGap) - $cellGap, $minInner)
        [pscustomobject]@{
            Module = $module; CountText = $countText; Columns = $columns
            Width = $inner + (2 * $cardPad); Height = 0
        }
    }
    $cards = @($cards | Sort-Object -Property @{ Expression = { $_.Module.Counts.total }; Descending = $true }, @{ Expression = { $_.Module.Name } })

    # Shelf-pack into rows.
    $rows = New-Object System.Collections.Generic.List[object]
    $row = New-Object System.Collections.Generic.List[object]
    $rowWidth = 0
    foreach ($card in $cards)
    {
        $needed = if ($row.Count -eq 0) { $card.Width } else { $rowWidth + $cardGap + $card.Width }
        if ($row.Count -gt 0 -and $needed -gt $usable)
        {
            $rows.Add($row)
            $row = New-Object System.Collections.Generic.List[object]
            $needed = $card.Width
        }
        $row.Add($card)
        $rowWidth = $needed
    }
    if ($row.Count -gt 0) { $rows.Add($row) }

    for ($r = 0; $r -lt $rows.Count; $r++)
    {
        $row = $rows[$r]
        $natural = ($row | Measure-Object -Property Width -Sum).Sum + ($cardGap * ($row.Count - 1))

        # Justify full rows (and a nearly full last row): share the slack by
        # natural width, then fit more cell columns into the wider cards.
        $isLast = $r -eq ($rows.Count - 1)
        if (-not $isLast -or ($natural / $usable) -ge 0.6)
        {
            $slack = $usable - $natural
            $naturalCards = $natural - ($cardGap * ($row.Count - 1))
            $given = 0
            for ($i = 0; $i -lt $row.Count; $i++)
            {
                $card = $row[$i]
                $extra = if ($i -eq $row.Count - 1) { $slack - $given } else { [int][math]::Floor($slack * $card.Width / $naturalCards) }
                $given += $extra
                $card.Width += $extra
                $fit = [int][math]::Floor(($card.Width - (2 * $cardPad) + $cellGap) / ($cell + $cellGap))
                $card.Columns = [math]::Max($card.Columns, [math]::Min($fit, $card.Module.Counts.total))
            }
        }

        $rowHeight = 0
        foreach ($card in $row)
        {
            $cellRows = [int][math]::Ceiling($card.Module.Counts.total / $card.Columns)
            $rowHeight = [math]::Max($rowHeight, $cardPad + $cardTitle + ($cellRows * ($cell + $cellGap)) - $cellGap + $cardPad)
        }

        $x = $margin
        foreach ($card in $row)
        {
            Add-Line ('  <g transform="translate({0},{1})">' -f $x, $y)
            Add-Line ('    <rect class="card" width="{0}" height="{1}" rx="8"/>' -f $card.Width, $rowHeight)
            Add-Line ('    <text class="label" x="{0}" y="{1}">{2}</text>' -f $cardPad, ($cardPad + 11), (ConvertTo-Xml $card.Module.Name))
            Add-Line ('    <text class="meta small" x="{0}" y="{1}" text-anchor="end">{2}</text>' -f ($card.Width - $cardPad), ($cardPad + 11), $card.CountText)

            $ordered = foreach ($status in $statusOrder) { $card.Module.Functions | Where-Object { $_.Status -eq $status } }
            $index = 0
            foreach ($function in $ordered)
            {
                $cx = $cardPad + (($index % $card.Columns) * ($cell + $cellGap))
                $cy = $cardPad + $cardTitle + ([math]::Floor($index / $card.Columns) * ($cell + $cellGap))
                $tip = ConvertTo-Xml ('{0}: {1}' -f $function.Name, $statusLabels[$function.Status])
                Add-Line ('    <rect class="{0}" x="{1}" y="{2}" width="{3}" height="{3}" rx="2"><title>{4}</title></rect>' -f $function.Status, $cx, $cy, $cell, $tip)
                $index++
            }
            Add-Line '  </g>'
            $x += $card.Width + $cardGap
        }
        $y += $rowHeight + $cardGap
    }
    $y += 30 - $cardGap
}

$height = $y - 30 + $margin
$svg = New-Object System.Text.StringBuilder
[void]$svg.Append(('<svg xmlns="http://www.w3.org/2000/svg" width="{0}" height="{1}" viewBox="0 0 {0} {1}" role="img" aria-label="OpenEra API coverage: {2}">' -f $width, $height, (ConvertTo-Xml $summary))).Append("`n")
[void]$svg.Append("  <style>`n")
[void]$svg.Append("    .bg{fill:#ffffff;stroke:#d0d7de}.card{fill:#f6f8fa;stroke:#d8dee4}.track{fill:#eaeef2}`n")
[void]$svg.Append("    .title{font:600 20px $font;fill:#1f2328}.h2{font:600 15px $font;fill:#1f2328}`n")
[void]$svg.Append("    .label{font:600 12px $font;fill:#1f2328}.meta{font:400 12px $font;fill:#59636e}.small{font-size:11px}`n")
[void]$svg.Append("    .implemented{fill:#1a7f37}.partial{fill:#bf8700}.stubbed{fill:#8250df}.missing{fill:#d0d7de}`n")
[void]$svg.Append("    @media (prefers-color-scheme:dark){.bg{fill:#0d1117;stroke:#30363d}.card{fill:#161b22;stroke:#30363d}.track{fill:#21262d}`n")
[void]$svg.Append("      .title,.h2,.label{fill:#e6edf3}.meta{fill:#9198a1}`n")
[void]$svg.Append("      .implemented{fill:#3fb950}.partial{fill:#d29922}.stubbed{fill:#a371f7}.missing{fill:#30363d}}`n")
[void]$svg.Append("  </style>`n")
[void]$svg.Append(('  <rect class="bg" x="0.5" y="0.5" width="{0}" height="{1}" rx="12"/>' -f ($width - 1), ($height - 1))).Append("`n")
[void]$svg.Append($body.ToString())
[void]$svg.Append("</svg>`n")
$svgText = $svg.ToString()

# ---------------------------------------------------------------- Markdown matrix
$md = New-Object System.Text.StringBuilder
function Add-Md([string]$Line) { [void]$md.Append($Line).Append("`n") }

Add-Md ('| Category | Module | {0} | {1} | {2} | {3} | Coverage |' -f $statusLabels.implemented, $statusLabels.partial, $statusLabels.stubbed, $statusLabels.missing)
Add-Md '| :--- | :--- | ---: | ---: | ---: | ---: | ---: |'
foreach ($category in $categories)
{
    foreach ($module in $category.Modules)
    {
        $c = $module.Counts
        Add-Md ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} |' -f $category.Name, $module.Name, $c.implemented, $c.partial, $c.stubbed, $c.missing, (Format-Percent $c.implemented $c.total))
    }
}
Add-Md ('| **Total** | | **{0}** | **{1}** | **{2}** | **{3}** | **{4}** |' -f $grand.implemented, $grand.partial, $grand.stubbed, $grand.missing, (Format-Percent $grand.implemented $grand.total))

foreach ($category in $categories)
{
    Add-Md ''
    Add-Md ('### {0}' -f $category.Name)
    foreach ($module in $category.Modules)
    {
        Add-Md ''
        Add-Md ('<details><summary><b>{0}</b>: {1}/{2} implemented</summary>' -f $module.Name, $module.Counts.implemented, $module.Counts.total)
        Add-Md ''
        Add-Md '| Function | Status |'
        Add-Md '| :--- | :--- |'
        foreach ($function in $module.Functions)
        {
            Add-Md ('| `{0}` | {1} {2} |' -f $function.Name, $statusEmoji[$function.Status], $statusLabels[$function.Status])
        }
        Add-Md ''
        Add-Md '</details>'
    }
}
$matrix = $md.ToString()

$beginMarker = '<!-- api-matrix:begin (generated by tools/Update-Compatibility.ps1) -->'
$endMarker = '<!-- api-matrix:end -->'
$markdown = ([IO.File]::ReadAllText($markdownPath, $utf8)).Replace("`r`n", "`n")
$begin = $markdown.IndexOf($beginMarker)
$end = $markdown.IndexOf($endMarker)
if ($begin -lt 0 -or $end -lt $begin)
{
    throw "COMPATIBILITY.md is missing the '$beginMarker' / '$endMarker' markers."
}
$newMarkdown = $markdown.Substring(0, $begin + $beginMarker.Length) + "`n" + $matrix + $markdown.Substring($end)

# ---------------------------------------------------------------- Write or check
$outputs = @(
    @{ Path = $svgPath; Text = $svgText },
    @{ Path = $markdownPath; Text = $newMarkdown }
)
$stale = @()
foreach ($output in $outputs)
{
    $current = if (Test-Path $output.Path) { ([IO.File]::ReadAllText($output.Path, $utf8)).Replace("`r`n", "`n") } else { '' }
    if ($current -ne $output.Text)
    {
        $stale += $output.Path
        if (-not $Check) { [IO.File]::WriteAllText($output.Path, $output.Text, $utf8) }
    }
}

if ($Check)
{
    if ($stale.Count -gt 0)
    {
        Write-Host "Out of date (run tools/Update-Compatibility.ps1):"
        $stale | ForEach-Object { Write-Host "  $_" }
        exit 1
    }
    Write-Host 'Compatibility docs are up to date.'
}
else
{
    Write-Host ('{0}/{1} functions implemented; updated {2} file(s).' -f $grand.implemented, $grand.total, $stale.Count)
}
