' guest-memory-poll.vbs - cross-check of the peak_rss_kb of loomora.exe: while a "loomora.exe
' --probe" started by the trial driver is alive (2 s), poll its memory counters through WMI
' (Win32_Process) every 300 ms, from another process. Read-only.
' Run in the guest with: cscript //nologo guest-memory-poll.vbs
Option Explicit
Dim wmi, items, item, i, seen
On Error Resume Next
Set wmi = GetObject("winmgmts:\\.\root\cimv2")
If Err.Number <> 0 Then
  WScript.Echo "WMI unavailable: " & Err.Description
  WScript.Quit 1
End If
seen = 0
For i = 1 To 8
  Set items = wmi.ExecQuery("Select * from Win32_Process Where Name='loomora.exe'")
  For Each item In items
    seen = seen + 1
    WScript.Echo "poll " & i & ": pid=" & item.ProcessId & " WorkingSetSize_bytes=" & item.WorkingSetSize & " (" & (item.WorkingSetSize / 1024) & " KB) PeakWorkingSetSize=" & item.PeakWorkingSetSize & " PageFileUsage=" & item.PageFileUsage & " PeakPageFileUsage=" & item.PeakPageFileUsage
  Next
  WScript.Sleep 300
Next
WScript.Echo "samples with a loomora.exe process: " & seen
