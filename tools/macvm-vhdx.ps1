<#
.SYNOPSIS
  Puts the macOS virtual machines on a Windows drive with room (ADR 0019) by creating an ext4 VHDX and
  mounting it in WSL2 as /mnt/wsl/macvm.

.DESCRIPTION
  Why: the WSL distribution disk (ext4.vhdx) may sit on a nearly full drive, and `df` inside WSL shows its
  virtual size, not the room left. A VHDX on another drive is native ext4 (fast qcow2 I/O, unlike /mnt/<drive>).

  Without -Apply nothing is changed: the script prints what it would do. With -Apply it needs an elevated
  PowerShell (`wsl --mount` and diskpart need administrator rights) and does, in order:
    1. -Create : creates the dynamically expanding VHDX (diskpart: create vdisk ... type=expandable) if absent;
    2. -Format : attaches it bare, formats the ONE new disk as ext4 (only if it has no filesystem and its size
                 matches -SizeGB), detaches it. Refuses otherwise;
    3. -Mount  : `wsl --mount --vhd <Path> --name macvm`, i.e. /mnt/wsl/macvm in every distribution.
  -Unmount runs `wsl --unmount <Path>`. The mount does not survive a reboot or `wsl --shutdown`: run -Mount again.
  The file grows only as the VMs write; it never shrinks by itself. Nothing is downloaded.

  Not run on the owner's machine yet when this was written: the first run is the owner's, with -Apply.

.EXAMPLE
  .\tools\macvm-vhdx.ps1 -Path H:\loomora-vm\macvm.vhdx -SizeGB 80            # dry run, prints the plan
  .\tools\macvm-vhdx.ps1 -Path H:\loomora-vm\macvm.vhdx -SizeGB 80 -Create -Format -Mount -Apply
  .\tools\macvm-vhdx.ps1 -Path H:\loomora-vm\macvm.vhdx -Mount -Apply        # after a reboot
#>
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$SizeGB = 80,
    [string]$Distro = 'Ubuntu-24.04',
    [switch]$Create, [switch]$Format, [switch]$Mount, [switch]$Unmount, [switch]$Apply
)
$ErrorActionPreference = 'Stop'
$Path = [System.IO.Path]::GetFullPath($Path)
$drive = Split-Path -Qualifier $Path
if (-not (Test-Path $drive)) { throw "drive $drive not found" }
$freeGB = [math]::Round((Get-PSDrive ($drive.TrimEnd(':'))).Free / 1GB, 1)
Write-Host "VHDX: $Path  (virtual size $SizeGB GB, grows on demand); $freeGB GB free on $drive"
if (-not ($Create -or $Format -or $Mount -or $Unmount)) { Write-Host 'No action chosen: use -Create, -Format, -Mount or -Unmount (and -Apply).'; return }

function Step($text, [scriptblock]$action) {
    if ($Apply) { Write-Host "+ $text"; & $action } else { Write-Host "[dry run] $text" }
}
if ($Apply) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { throw 'run this in an elevated PowerShell (Run as administrator) when using -Apply' }
}

if ($Create) {
    if (Test-Path $Path) { Write-Host "already exists: $Path (kept)" }
    else {
        if ($freeGB -lt 20) { throw "less than 20 GB free on $drive" }
        Step "create $Path ($SizeGB GB, expandable)" {
            New-Item -ItemType Directory -Force -Path (Split-Path $Path) | Out-Null
            $script = Join-Path ([System.IO.Path]::GetTempPath()) 'macvm-diskpart.txt'
            "create vdisk file=`"$Path`" maximum=$($SizeGB * 1024) type=expandable" | Set-Content -Encoding ascii $script
            diskpart /s $script
            Remove-Item $script
        }
    }
}

if ($Format) {
    Step "attach bare, format the new disk as ext4 label macvm, detach (refuses if it already has a filesystem)" {
        $before = wsl -d $Distro -u root -- lsblk -dn -o NAME
        wsl --mount --vhd $Path --bare
        $after = wsl -d $Distro -u root -- lsblk -dn -o NAME
        $new = @($after | Where-Object { $before -notcontains $_ })
        if ($new.Count -ne 1) { wsl --unmount $Path; throw "expected exactly one new disk, saw: $($new -join ', ')" }
        $dev = "/dev/$($new[0].Trim())"
        $info = wsl -d $Distro -u root -- lsblk -dnb -o SIZE,FSTYPE $dev
        $sizeBytes = [int64](($info -split '\s+')[0])
        $fstype = ($info -split '\s+') | Select-Object -Skip 1 -First 1
        if ($fstype) { wsl --unmount $Path; throw "$dev already has a filesystem ($fstype); refusing to format" }
        if ([math]::Abs($sizeBytes / 1GB - $SizeGB) -gt 1) { wsl --unmount $Path; throw "$dev is $([math]::Round($sizeBytes/1GB,1)) GB, expected $SizeGB; refusing to format" }
        wsl -d $Distro -u root -- mkfs.ext4 -q -L macvm $dev
        wsl --unmount $Path
    }
}

if ($Mount) {
    Step "wsl --mount --vhd $Path --name macvm   (then /mnt/wsl/macvm)" { wsl --mount --vhd $Path --name macvm }
    Write-Host "Then, once, in WSL:  sudo mkdir -p /mnt/wsl/macvm/vm && sudo chown `$USER /mnt/wsl/macvm/vm"
    Write-Host "and use it:           export LOOMORA_VM_DIR=/mnt/wsl/macvm/vm"
}
if ($Unmount) { Step "wsl --unmount $Path" { wsl --unmount $Path } }
