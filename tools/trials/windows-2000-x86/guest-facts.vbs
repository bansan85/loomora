' guest-facts.vbs - facts of the Windows 2000 trial guest, one "key=value" line each.
' Run in the guest with: cscript //nologo guest-facts.vbs
' Read-only: registry reads, WMI queries and file version queries; nothing is changed.
Option Explicit
On Error Resume Next
Dim sh, fso, wmi, items, item, name, value, keys, k, files, f, path, dpi

Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

Sub Fact(key, value)
  If IsNull(value) Then value = "<null>"
  If IsEmpty(value) Then value = "<empty>"
  WScript.Echo key & "=" & value
End Sub

Function Reg(path)
  Err.Clear
  Reg = sh.RegRead(path)
  If Err.Number <> 0 Then
    Reg = "<absent>"
    Err.Clear
  End If
End Function

Fact "wsh.version", WScript.Version
Fact "wsh.engine", ScriptEngine & " " & ScriptEngineMajorVersion & "." & ScriptEngineMinorVersion & "." & ScriptEngineBuildVersion

keys = Array( _
  "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProductName", _
  "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\CurrentVersion", _
  "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\CurrentBuildNumber", _
  "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\CurrentType", _
  "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\CSDVersion", _
  "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\SystemRoot", _
  "HKLM\SYSTEM\CurrentControlSet\Control\Windows\CSDVersion", _
  "HKLM\SYSTEM\CurrentControlSet\Control\Nls\CodePage\ACP", _
  "HKLM\SYSTEM\CurrentControlSet\Control\Nls\CodePage\OEMCP", _
  "HKLM\SYSTEM\CurrentControlSet\Control\Nls\Language\Default", _
  "HKLM\SYSTEM\CurrentControlSet\Control\Nls\Language\InstallLanguage", _
  "HKCU\Keyboard Layout\Preload\1", _
  "HKEY_USERS\.DEFAULT\Keyboard Layout\Preload\1", _
  "HKCU\Control Panel\International\Locale", _
  "HKCU\Control Panel\International\sLanguage", _
  "HKCU\Control Panel\International\sCountry", _
  "HKCU\Control Panel\Accessibility\HighContrast\Flags", _
  "HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme", _
  "HKLM\SOFTWARE\Microsoft\Internet Explorer\Version")
For Each k In keys
  Fact "reg." & k, Reg(k)
Next

Set wmi = GetObject("winmgmts:\\.\root\cimv2")
If Err.Number <> 0 Then
  Fact "wmi", "unavailable: " & Err.Description
  Err.Clear
Else
  Set items = wmi.ExecQuery("Select * from Win32_OperatingSystem")
  For Each item In items
    Fact "wmi.os.Caption", item.Caption
    Fact "wmi.os.Version", item.Version
    Fact "wmi.os.BuildNumber", item.BuildNumber
    Fact "wmi.os.CSDVersion", item.CSDVersion
    Fact "wmi.os.ServicePackMajorVersion", item.ServicePackMajorVersion
    Fact "wmi.os.OSLanguage", item.OSLanguage
    Fact "wmi.os.CountryCode", item.CountryCode
    Fact "wmi.os.Locale", item.Locale
    Fact "wmi.os.CodeSet", item.CodeSet
    Fact "wmi.os.TotalVisibleMemorySize_KB", item.TotalVisibleMemorySize
    Fact "wmi.os.FreePhysicalMemory_KB", item.FreePhysicalMemory
  Next
  Set items = wmi.ExecQuery("Select * from Win32_ComputerSystem")
  For Each item In items
    Fact "wmi.cs.Manufacturer", item.Manufacturer
    Fact "wmi.cs.Model", item.Model
    Fact "wmi.cs.TotalPhysicalMemory_bytes", item.TotalPhysicalMemory
  Next
  Set items = wmi.ExecQuery("Select * from Win32_Processor")
  For Each item In items
    Fact "wmi.cpu.Name", item.Name
    Fact "wmi.cpu.Manufacturer", item.Manufacturer
    Fact "wmi.cpu.MaxClockSpeed_MHz", item.MaxClockSpeed
  Next
  Set items = wmi.ExecQuery("Select * from Win32_VideoController")
  For Each item In items
    Fact "wmi.video.Caption", item.Caption
    Fact "wmi.video.Resolution", item.CurrentHorizontalResolution & "x" & item.CurrentVerticalResolution & "x" & item.CurrentBitsPerPixel
  Next
  Set items = wmi.ExecQuery("Select * from Win32_DisplayConfiguration")
  For Each item In items
    Fact "wmi.display.LogPixels", item.LogPixels
  Next
  Err.Clear
End If

files = Array("kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ntdll.dll", "psapi.dll", "msvcrt.dll", "comctl32.dll", "shell32.dll", "dwmapi.dll", "uxtheme.dll")
For Each f In files
  path = sh.ExpandEnvironmentStrings("%SystemRoot%") & "\system32\" & f
  If fso.FileExists(path) Then
    Fact "file." & f, "version " & fso.GetFileVersion(path) & ", " & fso.GetFile(path).Size & " bytes, modified " & fso.GetFile(path).DateLastModified
  Else
    Fact "file." & f, "<absent>"
  End If
Next
