#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Prepares the Windows host for VirtualBox and creates the Mac OS X 10.9 Mavericks VM.

.PARAMETER Mode
    Disable : turns off what blocks VirtualBox (Hyper-V, VirtualMachinePlatform, VBS,
              memory integrity, Fast Startup, hypervisor launch at boot). The current
              settings are saved once to hyperv-state.json before the first change.
    Enable  : puts the settings saved by -Mode Disable back (WSL versions included).
              Windows defaults are used if no saved file exists.
    Create  : creates the VM in E:\data\vm\osxmavericks and starts it, only if the
              Windows hypervisor is off. Windows settings are not changed.

.NOTES
    Disable and Enable change Windows features and boot settings: a full restart
    (Restart, not Shut down) is required before the change takes effect.
    The VM is never deleted except when its own creation fails.
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Disable', 'Enable', 'Create')]
    [string]$Mode
)

$ErrorActionPreference = 'Stop'

$VBoxManage     = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$VmName         = 'OSXMavericks'
$BaseFolder     = 'E:\data\vm'
$VmDir          = Join-Path $BaseFolder 'osxmavericks'
$Installer      = Join-Path $VmDir 'OSXMavericks10.9.5.iso'
$InstallerVdi   = Join-Path $VmDir 'OSXMavericks-installer.vdi'
$DiskPath       = Join-Path $VmDir "$VmName.vdi"
$StateFile      = Join-Path $VmDir 'hyperv-state.json'
$DiskMB         = 40960
$OsType         = 'MacOS109_64'

$Features       = @('Microsoft-Hyper-V-All', 'VirtualMachinePlatform')
$DeviceGuardKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard'
$HvciKey        = "$DeviceGuardKey\Scenarios\HypervisorEnforcedCodeIntegrity"
$PowerKey       = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power'
$LxssKey        = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss'

function Invoke-VBox {
    & $VBoxManage @args
    if ($LASTEXITCODE -ne 0) {
        throw "VBoxManage $($args -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Invoke-Checked {
    $exe  = $args[0]
    $rest = @($args | Select-Object -Skip 1)
    & $exe @rest
    if ($LASTEXITCODE -ne 0) {
        throw "$exe $($rest -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Test-Iso9660 {
    param([string]$Path)
    $fs = [IO.File]::OpenRead($Path)
    try {
        $buf = New-Object byte[] 5
        $fs.Seek(0x8001, 'Begin') | Out-Null
        $null = $fs.Read($buf, 0, 5)
        return ([Text.Encoding]::ASCII.GetString($buf) -eq 'CD001')
    } finally {
        $fs.Close()
    }
}

function Get-FeatureState {
    param([string]$Name)
    (Get-WindowsOptionalFeature -Online -FeatureName $Name).State.ToString()
}

function Get-DWord {
    param([string]$Path, [string]$Name)
    if (-not (Test-Path $Path)) { return $null }
    $prop = Get-ItemProperty -Path $Path -Name $Name -ErrorAction SilentlyContinue
    if ($null -eq $prop) { return $null }
    return $prop.$Name
}

function Set-DWord {
    param([string]$Path, [string]$Name, [int]$Value)
    if (-not (Test-Path $Path)) { New-Item -Path $Path -Force | Out-Null }
    New-ItemProperty -Path $Path -Name $Name -Value $Value -PropertyType DWord -Force | Out-Null
}

function Remove-DWord {
    param([string]$Path, [string]$Name)
    if (Test-Path $Path) { Remove-ItemProperty -Path $Path -Name $Name -ErrorAction SilentlyContinue }
}

function Get-WslDistros {
    # name -> WSL version (1 or 2), read from the per-user Lxss registry key
    $distros = [ordered]@{}
    if (Test-Path $LxssKey) {
        foreach ($item in Get-ChildItem $LxssKey) {
            $p = Get-ItemProperty $item.PSPath
            if ($p.DistributionName) { $distros[[string]$p.DistributionName] = [int]$p.Version }
        }
    }
    return $distros
}

function Get-CurrentSettings {
    # Note: PowerShell variable names are case-insensitive, so this must not be named $features
    $featureStates = [ordered]@{}
    foreach ($f in $Features) { $featureStates[$f] = Get-FeatureState $f }
    [ordered]@{
        savedAt     = (Get-Date).ToString('s')
        features    = $featureStates
        vbs         = Get-DWord $DeviceGuardKey 'EnableVirtualizationBasedSecurity'
        hvci        = Get-DWord $HvciKey 'Enabled'
        fastStartup = Get-DWord $PowerKey 'HiberbootEnabled'
        wslDistros  = Get-WslDistros
    }
}

# Mode Disable: turn off everything that blocks VirtualBox
function Invoke-Disable {
    if (-not (Test-Path $VmDir)) { New-Item -ItemType Directory -Path $VmDir | Out-Null }

    if (-not (Test-Path $StateFile)) {
        Get-CurrentSettings | ConvertTo-Json -Depth 5 | Set-Content -Path $StateFile -Encoding UTF8
        Write-Host "[1/3] Current settings saved to $StateFile"
    } else {
        Write-Host "[1/3] Saved settings already present, keeping them: $StateFile"
    }

    Write-Host '[2/3] Disabling Windows features...'
    foreach ($f in $Features) {
        if ((Get-FeatureState $f) -ne 'Disabled') {
            Write-Host "      Disabling $f"
            Disable-WindowsOptionalFeature -Online -FeatureName $f -NoRestart | Out-Null
        }
    }

    Write-Host '[3/3] Disabling hypervisor launch, VBS, memory integrity and Fast Startup...'
    Invoke-Checked bcdedit /set hypervisorlaunchtype off | Out-Null
    Set-DWord $DeviceGuardKey 'EnableVirtualizationBasedSecurity' 0
    if (Test-Path $HvciKey) { Set-DWord $HvciKey 'Enabled' 0 }
    Set-DWord $PowerKey 'HiberbootEnabled' 0
    # WSL distributions are not changed by this mode.
}

# Mode Enable: put the saved settings back
function Invoke-Enable {
    $wanted = @{}
    if (Test-Path $StateFile) {
        $s = Get-Content $StateFile -Raw | ConvertFrom-Json
        Write-Host "Restoring settings saved on $($s.savedAt)"
        foreach ($prop in $s.wslDistros.PSObject.Properties) { $wanted[$prop.Name] = [int]$prop.Value }
        $featureWanted = $s.features
        $vbsWanted     = $s.vbs
        $hvciWanted    = $s.hvci
        $fastWanted    = $s.fastStartup
    } else {
        Write-Warning 'No saved settings found: restoring Windows defaults.'
        foreach ($name in (Get-WslDistros).Keys) { $wanted[$name] = 2 }
        $featureWanted = [pscustomobject]@{ 'Microsoft-Hyper-V-All' = 'Enabled'; 'VirtualMachinePlatform' = 'Enabled' }
        $vbsWanted     = 1
        $hvciWanted    = 1
        $fastWanted    = 1
    }

    foreach ($f in $Features) {
        if ($featureWanted.$f -eq 'Enabled' -and (Get-FeatureState $f) -ne 'Enabled') {
            Write-Host "      Enabling $f"
            Enable-WindowsOptionalFeature -Online -FeatureName $f -All -NoRestart | Out-Null
        }
    }

    Invoke-Checked bcdedit /set hypervisorlaunchtype auto | Out-Null

    if ($null -eq $vbsWanted) { Remove-DWord $DeviceGuardKey 'EnableVirtualizationBasedSecurity' }
    else { Set-DWord $DeviceGuardKey 'EnableVirtualizationBasedSecurity' ([int]$vbsWanted) }

    if ($null -ne $hvciWanted) { Set-DWord $HvciKey 'Enabled' ([int]$hvciWanted) }

    if ($null -eq $fastWanted) { Remove-DWord $PowerKey 'HiberbootEnabled' }
    else { Set-DWord $PowerKey 'HiberbootEnabled' ([int]$fastWanted) }

    if (Get-Command wsl -ErrorAction SilentlyContinue) {
        $current = Get-WslDistros
        foreach ($name in $wanted.Keys) {
            if ($current.Contains($name) -and $current[$name] -ne $wanted[$name]) {
                Write-Host "      Setting WSL distribution $name to version $($wanted[$name])"
                Invoke-Checked wsl --set-version $name $wanted[$name]
            }
        }
    }
}

# Mode Create: create the VM and start it when the hypervisor is off
function Invoke-Create {
    if (-not (Test-Path $VBoxManage)) { throw "VBoxManage not found: $VBoxManage" }
    if (-not (Test-Path $Installer))  { throw "Installer image not found: $Installer" }

    & $VBoxManage showvminfo $VmName --machinereadable *> $null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[1/3] VM '$VmName' already exists, skipping creation."
    } else {
        Write-Host "[1/3] Creating VM '$VmName' in $VmDir ..."
        try {
            Invoke-VBox createvm --name $VmName --ostype $OsType --basefolder $BaseFolder --register

            Write-Host "      Creating virtual disk ($DiskMB MB, dynamic VDI)..."
            Invoke-VBox createmedium disk --filename $DiskPath --size $DiskMB --format VDI --variant Standard

            # The installer is an ISO 9660 CD image, or a raw GPT disk image that must be converted
            $isIso9660 = Test-Iso9660 $Installer
            if ($isIso9660) {
                Write-Host '      Installer is an ISO 9660 image: attaching it as a DVD.'
                $bootOrder = @('--boot1', 'dvd', '--boot2', 'disk')
            } else {
                Write-Host '      Installer is a raw disk image: converting to VDI (large copy, please wait)...'
                if (-not (Test-Path $InstallerVdi)) {
                    Invoke-VBox convertfromraw $Installer $InstallerVdi --format VDI
                }
                $bootOrder = @('--boot1', 'disk', '--boot2', 'none')
            }

            Write-Host '      Configuring hardware...'
            Invoke-VBox modifyvm $VmName `
                --memory 4096 --cpus 2 `
                --firmware efi64 --chipset ich9 `
                --mouse usbtablet --keyboard usb `
                --graphicscontroller vmsvga --vram 128 --accelerate3d off `
                --hwvirtex on --nested-paging on --ioapic on --rtcuseutc on `
                @bootOrder --boot3 none --boot4 none

            Invoke-VBox storagectl $VmName --name SATA --add sata --controller IntelAhci --portcount 2
            Invoke-VBox storageattach $VmName --storagectl SATA --port 0 --device 0 --type hdd --medium $DiskPath
            if ($isIso9660) {
                Invoke-VBox storageattach $VmName --storagectl SATA --port 1 --device 0 --type dvddrive --medium $Installer
            } else {
                Invoke-VBox storageattach $VmName --storagectl SATA --port 1 --device 0 --type hdd --medium $InstallerVdi
            }

            # Mac hardware identification (EFI firmware and SMC)
            $extraData = [ordered]@{
                'VBoxInternal/Devices/efi/0/Config/DmiSystemProduct'  = 'MacBookPro11,3'
                'VBoxInternal/Devices/efi/0/Config/DmiSystemVersion'  = '1.0'
                'VBoxInternal/Devices/efi/0/Config/DmiBoardProduct'   = 'Iloveapple'
                'VBoxInternal/Devices/smc/0/Config/DeviceKey'         = 'ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc'
                'VBoxInternal/Devices/smc/0/Config/GetKeyFromRealSMC' = '0'
            }
            foreach ($key in $extraData.Keys) {
                Invoke-VBox setextradata $VmName $key $extraData[$key]
            }
        } catch {
            Write-Warning 'VM creation failed: removing the partial VM before stopping.'
            & $VBoxManage unregistervm $VmName --delete *> $null
            throw
        }
    }

    $configLine = (& $VBoxManage showvminfo $VmName --machinereadable) | Where-Object { $_ -like 'CfgFile=*' }
    Write-Host "[2/3] $configLine"

    $hypervisorActive = (Get-CimInstance Win32_ComputerSystem).HypervisorPresent
    if ($hypervisorActive) {
        Write-Warning 'The Windows hypervisor is still running. Run -Mode Disable, restart the PC (not Shut down), then run -Mode Create again to start the VM.'
        exit 2
    }

    $vmState = (& $VBoxManage showvminfo $VmName --machinereadable) | Where-Object { $_ -like 'VMState=*' }
    if ($vmState -like '*"running"*') {
        Write-Host "[3/3] VM '$VmName' is already running."
        return
    }

    Write-Host "[3/3] Starting VM '$VmName'..."
    Invoke-VBox startvm $VmName --type gui
}

switch ($Mode) {
    'Disable' {
        Invoke-Disable
        Write-Host 'Done. Restart the PC (not Shut down) for the changes to take effect.'
    }
    'Enable' {
        Invoke-Enable
        Write-Host 'Done. Restart the PC (not Shut down) for the changes to take effect.'
    }
    'Create' {
        Invoke-Create
    }
}
