# Typing check of the Windows Modern UI (WinUI 3) through UI Automation (Windows PowerShell 5.1).
#   powershell -NoProfile -ExecutionPolicy Bypass -File winui-typing.ps1 -Exe <Loomora.WinUI.exe> -Text Marie
#
# Tooling, not evidence. Starts the UI normally (no --probe), waits for its window, reads the field
# and the greeting through UI Automation, selects the content of the field, types Text with
# SendKeys (real keyboard events to the focused field; the method is reported), measures the time
# until the greeting reads "Bonjour " + Text, closes the window (CloseMainWindow, WM_CLOSE) and
# reports the exit code. One JSON object on standard output.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Text
)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]

function Find-Descendants($element, $type) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $type)
    return $element.FindAll($TS::Descendants, $cond)
}

$clock = [System.Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath $Exe -PassThru -WorkingDirectory (Split-Path -Parent $Exe)
$pidCondition = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $process.Id)
$window = $null
while ($null -eq $window -and $clock.Elapsed.TotalSeconds -lt 30) {
    $window = $AE::RootElement.FindFirst($TS::Children, $pidCondition)
    if ($null -eq $window) { Start-Sleep -Milliseconds 20 }
}
if ($null -eq $window) { $process.Kill(); throw "no window within 30 seconds" }
$windowAfterMs = $clock.ElapsedMilliseconds

$edit = $null
$greeting = $null
while ($clock.Elapsed.TotalSeconds -lt 30) {
    $edits = Find-Descendants $window $CT::Edit
    if ($edits.Count -gt 0) { $edit = $edits[0] }
    $texts = Find-Descendants $window $CT::Text
    foreach ($t in $texts) { if ($t.Current.Name -like "Bonjour*") { $greeting = $t } }
    if ($null -ne $edit -and $null -ne $greeting) { break }
    Start-Sleep -Milliseconds 20
}
if ($null -eq $edit -or $null -eq $greeting) { $process.Kill(); throw "field or greeting not found" }

$valuePattern = $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
$result = [ordered]@{
    window_after_ms = $windowAfterMs
    initial_field   = $valuePattern.Current.Value
    initial_greeting = $greeting.Current.Name
}

$method = "SendKeys"
$edit.SetFocus()
Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait("^a")
Start-Sleep -Milliseconds 100
$typing = [System.Diagnostics.Stopwatch]::StartNew()
[System.Windows.Forms.SendKeys]::SendWait($Text)
$expected = "Bonjour " + $Text
$shown = ""
while ($typing.Elapsed.TotalSeconds -lt 5) {
    $shown = ($AE::RootElement.FindFirst($TS::Children, $pidCondition)).FindFirst($TS::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $expected)))
    if ($null -ne $shown) { break }
    Start-Sleep -Milliseconds 10
}
$typedMs = $typing.ElapsedMilliseconds
if ($null -eq $shown) {
    # the keyboard events did not reach the field: set the value through UI Automation instead
    $method = "ValuePattern.SetValue"
    $typing.Restart()
    $valuePattern.SetValue($Text)
    while ($typing.Elapsed.TotalSeconds -lt 5) {
        $shown = $window.FindFirst($TS::Descendants,
            (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $expected)))
        if ($null -ne $shown) { break }
        Start-Sleep -Milliseconds 10
    }
    $typedMs = $typing.ElapsedMilliseconds
}
$result["method"] = $method
$result["typed_text"] = $Text
$result["field_after"] = $valuePattern.Current.Value
$result["greeting_after"] = if ($null -ne $shown) { $shown.Current.Name } else { "" }
$result["greeting_matches"] = ($null -ne $shown)
$result["typing_to_greeting_ms"] = $typedMs

$process.Refresh()
[void]$process.CloseMainWindow()   # WM_CLOSE to the main window, like the close button
$exited = $process.WaitForExit(15000)
$result["exited"] = $exited
$result["exit_code"] = if ($exited) { $process.ExitCode } else { $null }
if (-not $exited) { $process.Kill() }
$result | ConvertTo-Json
