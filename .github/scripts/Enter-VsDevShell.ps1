# Loads the newest Visual Studio x64 developer environment (cl, clang-cl, CMake, Ninja).
$ErrorActionPreference = 'Stop'

$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$env:PATH = "$installer;$env:PATH" # VsDevCmd calls vswhere by name.

$vs = & "$installer\vswhere.exe" -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs)
{
    throw 'Visual Studio with the x64 C++ tools was not found.'
}

Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -DevCmdArguments '-arch=x64 -host_arch=x64' -SkipAutomaticLocation | Out-Null

# Appended, so CMake still finds cl.exe first.
$VsLlvmBin = "$vs\VC\Tools\Llvm\x64\bin"
$env:PATH = "$env:PATH;$VsLlvmBin"

# Visual Studio's own clang-cl; a standalone LLVM earlier on PATH may be too old for this STL.
$VsClangCl = Join-Path $VsLlvmBin 'clang-cl.exe'
