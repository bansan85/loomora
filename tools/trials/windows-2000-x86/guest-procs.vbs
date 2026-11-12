' guest-procs.vbs - lists the loomora.exe, runw.exe and loomora_selftest*.exe processes of the Windows 2000 trial guest
' (Windows 2000 has no tasklist or taskkill), with their memory counters, through WMI.
' Run in the guest with: cscript //nologo guest-procs.vbs
' Output: "PROCESSES <count>", then one "PROCESS name pid WorkingSet_KB ..." line per process.
' Read-only.
Option Explicit
Dim wmi, items, item, n
On Error Resume Next
Set wmi = GetObject("winmgmts:\\.\root\cimv2")
If Err.Number <> 0 Then
  WScript.Echo "PROCESSES unknown (WMI: " & Err.Description & ")"
  WScript.Quit 1
End If
Set items = wmi.ExecQuery("Select * from Win32_Process Where Name='loomora.exe' Or Name='runw.exe' Or Name='loomora_selftest.exe' Or Name='loomora_selftest-noshim.exe'")
n = 0
For Each item In items
  n = n + 1
Next
WScript.Echo "PROCESSES " & n
For Each item In items
  WScript.Echo "PROCESS " & item.Name & " pid=" & item.ProcessId & " WorkingSetSize_KB=" & (item.WorkingSetSize / 1024) & " PageFileUsage_B=" & item.PageFileUsage
Next
