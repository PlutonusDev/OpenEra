<#
.SYNOPSIS
    Adds the console-only (GDKX) APIs a title was seen calling to
    docs/compatibility/gdkx-status.json, from an openera log, then regenerates
    the GDKX coverage image and matrix.

.DESCRIPTION
    Reads the unimplemented-call lines of an openera log (the summary, or the
    "Unimplemented ..." warnings). It keeps calls into a console module listed
    under "consoleModules" in gdkx-status.json, and Era API or NT syscall stubs
    from any module. New functions are added as missing. A function already
    listed keeps its status and gains the report as a source.

    Names come only from what a title imports, which is observable behaviour.
    Implementing a function still needs a public source for what it does, or
    observed behaviour (see CONTRIBUTING.md).

.PARAMETER Log
    An openera log, or the summary pasted from a compatibility report.

.PARAMETER Source
    Where it came from, e.g. "#64" for a compatibility report issue.

.PARAMETER WhatIf
    List what would be added without writing anything.
#>
param(
    [Parameter(Mandatory)][string]$Log,
    [Parameter(Mandatory)][string]$Source,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$statusPath = Join-Path $root 'docs/compatibility/gdkx-status.json'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$otherCategory = 'Other console modules'

function Get-ModuleKey([string]$Name)
{
    $file = ($Name -split '[\\/]')[-1].ToLowerInvariant()
    return ($file -replace '\.(dll|exe)$', '')
}

# ---------------------------------------------------------------- Read the log
$pattern = '\b(Win32Import|GameCoreApi|NtSyscall|EraApi)\s+([^!\s]+)!(\S+)'
$seen = [ordered]@{}
foreach ($line in [IO.File]::ReadAllLines((Resolve-Path $Log)))
{
    foreach ($match in [regex]::Matches($line, $pattern))
    {
        $kind = $match.Groups[1].Value
        $module = $match.Groups[2].Value
        $symbol = $match.Groups[3].Value
        $id = '{0}!{1}' -f (Get-ModuleKey $module), $symbol
        if (-not $seen.Contains($id))
        {
            $seen[$id] = [pscustomobject]@{ Kind = $kind; Module = $module; Symbol = $symbol }
        }
    }
}

# ---------------------------------------------------------------- Merge
$data = [IO.File]::ReadAllText($statusPath, $utf8) | ConvertFrom-Json
$consoleModules = @{}
foreach ($property in $data.consoleModules.PSObject.Properties)
{
    $consoleModules[$property.Name.ToLowerInvariant()] = [string]$property.Value
}

# Plain, ordered copies, so the file can be written back deterministically.
$categories = New-Object System.Collections.Generic.List[object]
foreach ($category in @($data.categories))
{
    $modules = New-Object System.Collections.Generic.List[object]
    foreach ($module in @($category.modules))
    {
        $functions = [ordered]@{}
        foreach ($property in $module.functions.PSObject.Properties) { $functions[$property.Name] = [string]$property.Value }
        $sources = [ordered]@{}
        if ($module.PSObject.Properties['sources'])
        {
            foreach ($property in $module.sources.PSObject.Properties) { $sources[$property.Name] = @($property.Value) }
        }
        $modules.Add([pscustomobject]@{ Name = [string]$module.name; Functions = $functions; Sources = $sources })
    }
    $categories.Add([pscustomobject]@{ Name = [string]$category.name; Modules = $modules })
}

function Get-Module([string]$CategoryName, [string]$ModuleName)
{
    $key = Get-ModuleKey $ModuleName
    foreach ($category in $categories)
    {
        foreach ($module in $category.Modules)
        {
            if ((Get-ModuleKey $module.Name) -eq $key) { return $module }
        }
    }
    $category = $categories | Where-Object { $_.Name -eq $CategoryName } | Select-Object -First 1
    if ($null -eq $category)
    {
        $category = [pscustomobject]@{ Name = $CategoryName; Modules = (New-Object System.Collections.Generic.List[object]) }
        $categories.Add($category)
    }
    $module = [pscustomobject]@{ Name = ($ModuleName -replace '\.(dll|exe)$', ''); Functions = [ordered]@{}; Sources = [ordered]@{} }
    $category.Modules.Add($module)
    return $module
}

$added = New-Object System.Collections.Generic.List[string]
$sourced = 0
foreach ($call in $seen.Values)
{
    $key = Get-ModuleKey $call.Module
    if ($consoleModules.ContainsKey($key)) { $categoryName = $consoleModules[$key] }
    elseif ($call.Kind -eq 'EraApi' -or $call.Kind -eq 'NtSyscall') { $categoryName = $otherCategory }
    else { continue } # Desktop Windows and PC GDK APIs are tracked in api-status.json.

    $module = Get-Module $categoryName $call.Module
    if (-not $module.Functions.Contains($call.Symbol))
    {
        $module.Functions[$call.Symbol] = 'missing'
        $added.Add(('{0}!{1}' -f $module.Name, $call.Symbol))
    }
    $list = New-Object System.Collections.Generic.List[string]
    if ($module.Sources.Contains($call.Symbol))
    {
        foreach ($entry in @($module.Sources[$call.Symbol])) { $list.Add([string]$entry) }
    }
    if (-not $list.Contains($Source))
    {
        $list.Add($Source)
        $sourced++
    }
    $module.Sources[$call.Symbol] = $list.ToArray()
}

Write-Host ('{0} console-only call(s) in the log: {1} new, {2} newly sourced to {3}.' -f `
        ($seen.Values | Where-Object { $consoleModules.ContainsKey((Get-ModuleKey $_.Module)) -or $_.Kind -in @('EraApi', 'NtSyscall') }).Count, `
        $added.Count, $sourced, $Source)
$added | ForEach-Object { Write-Host "  + $_" }
if ($WhatIf) { return }

# ---------------------------------------------------------------- Write back
function ConvertTo-JsonString([string]$Text)
{
    $escaped = $Text.Replace('\', '\\').Replace('"', '\"')
    return '"' + $escaped + '"'
}

$out = New-Object System.Text.StringBuilder
function Add-Json([string]$Line) { [void]$out.Append($Line).Append("`n") }

Add-Json '{'
Add-Json ('  "emptyText": {0},' -f (ConvertTo-JsonString $data.emptyText))
Add-Json '  "consoleModules": {'
$names = @($data.consoleModules.PSObject.Properties | ForEach-Object { $_.Name })
for ($i = 0; $i -lt $names.Count; $i++)
{
    $comma = if ($i -lt $names.Count - 1) { ',' } else { '' }
    Add-Json ('    {0}: {1}{2}' -f (ConvertTo-JsonString $names[$i]), (ConvertTo-JsonString $data.consoleModules.($names[$i])), $comma)
}
Add-Json '  },'
if ($categories.Count -eq 0)
{
    Add-Json '  "categories": []'
}
else
{
    Add-Json '  "categories": ['
    for ($c = 0; $c -lt $categories.Count; $c++)
    {
        $category = $categories[$c]
        Add-Json '    {'
        Add-Json ('      "name": {0},' -f (ConvertTo-JsonString $category.Name))
        Add-Json '      "modules": ['
        for ($m = 0; $m -lt $category.Modules.Count; $m++)
        {
            $module = $category.Modules[$m]
            Add-Json '        {'
            Add-Json ('          "name": {0},' -f (ConvertTo-JsonString $module.Name))
            Add-Json '          "functions": {'
            $functionNames = @($module.Functions.Keys)
            for ($f = 0; $f -lt $functionNames.Count; $f++)
            {
                $comma = if ($f -lt $functionNames.Count - 1) { ',' } else { '' }
                Add-Json ('            {0}: {1}{2}' -f (ConvertTo-JsonString $functionNames[$f]), (ConvertTo-JsonString $module.Functions[$functionNames[$f]]), $comma)
            }
            Add-Json '          },'
            Add-Json '          "sources": {'
            $sourceNames = @($module.Sources.Keys)
            for ($s = 0; $s -lt $sourceNames.Count; $s++)
            {
                $comma = if ($s -lt $sourceNames.Count - 1) { ',' } else { '' }
                $list = (@($module.Sources[$sourceNames[$s]]) | ForEach-Object { ConvertTo-JsonString $_ }) -join ', '
                Add-Json ('            {0}: [{1}]{2}' -f (ConvertTo-JsonString $sourceNames[$s]), $list, $comma)
            }
            Add-Json '          }'
            $comma = if ($m -lt $category.Modules.Count - 1) { ',' } else { '' }
            Add-Json ('        }}{0}' -f $comma)
        }
        Add-Json '      ]'
        $comma = if ($c -lt $categories.Count - 1) { ',' } else { '' }
        Add-Json ('    }}{0}' -f $comma)
    }
    Add-Json '  ]'
}
Add-Json '}'
[IO.File]::WriteAllText($statusPath, $out.ToString(), $utf8)

& (Join-Path $PSScriptRoot 'Update-GdkxCoverage.ps1')
