# Installs the Windows App Runtime that the unpackaged, framework-dependent WinUI 3 executable
# needs to start (package family Microsoft.WindowsAppRuntime.2), for one architecture, on a
# GitHub-hosted Windows runner (the images do not carry it). The packages are the MSIX files that
# the NuGet package Microsoft.WindowsAppSDK.Runtime ships in tools\MSIX\win10-<arch>; the version
# is the one ui/modern/windows-winui3/Loomora.WinUI.csproj pins, so the two cannot drift apart.
#
# Usage (Windows PowerShell 5.1, the one that has the Appx module):
#   powershell -File .github/scripts/install-windows-app-runtime.ps1 -Arch x64|arm64
param(
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Arch,
    [string]$Project = 'ui/modern/windows-winui3/Loomora.WinUI.csproj'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$match = Select-String -Path $Project -Pattern 'Microsoft\.WindowsAppSDK\.Runtime" Version="([0-9][0-9.]*)"'
if (-not $match) { throw "no Microsoft.WindowsAppSDK.Runtime version in $Project" }
$version = $match.Matches[0].Groups[1].Value

$work = Join-Path $env:RUNNER_TEMP "windows-app-runtime-$Arch"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$archive = Join-Path $work 'runtime.zip'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.windowsappsdk.runtime/$version/microsoft.windowsappsdk.runtime.$version.nupkg"
Write-Host "downloading $url"
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
Expand-Archive -Path $archive -DestinationPath (Join-Path $work 'pkg') -Force

$dir = Join-Path $work "pkg\tools\MSIX\win10-$Arch"
Write-Host 'installed before:'
Get-AppxPackage -Name 'Microsoft.WindowsAppRuntime*' |
    Select-Object Name, Version, Architecture | Format-Table -AutoSize | Out-String | Write-Host

# The framework package first, then the others (they depend on it). A hosted image has a Windows
# component that uses the framework (the Widgets/WebExperience package): -ForceApplicationShutdown
# lets the update close it instead of failing with 0x80073D02.
foreach ($name in 'Microsoft.WindowsAppRuntime.2', 'Microsoft.WindowsAppRuntime.Main.2',
                  'Microsoft.WindowsAppRuntime.Singleton.2', 'Microsoft.WindowsAppRuntime.DDLM.2') {
    $file = Join-Path $dir "$name.msix"
    Write-Host "installing $file"
    Add-AppxPackage -Path $file -ForceApplicationShutdown
}
Get-AppxPackage -Name 'Microsoft.WindowsAppRuntime*' |
    Select-Object Name, Version, Architecture | Format-Table -AutoSize | Out-String | Write-Host
