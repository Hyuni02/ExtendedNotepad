# .etxt 파일 연결 등록 / 해제. HKCU만 쓰므로 관리자 권한이 필요 없다.
#   powershell -ExecutionPolicy Bypass -File register.ps1
#   powershell -ExecutionPolicy Bypass -File register.ps1 -Unregister
param(
    [switch]$Unregister,
    [string]$ExePath
)

$ErrorActionPreference = 'Stop'

$progId  = 'ExtendedNotepad.Document'
$classes = 'HKCU:\Software\Classes'

if ($Unregister) {
    foreach ($k in "$classes\$progId", "$classes\.etxt") {
        if (Test-Path $k) { Remove-Item $k -Recurse -Force; "removed $k" }
    }
} else {
    if (-not $ExePath) {
        $ExePath = Join-Path $PSScriptRoot 'build\ExtendedNotepad.exe'
    }
    $ExePath = (Resolve-Path $ExePath).Path
    if (-not (Test-Path $ExePath)) { throw "exe not found: $ExePath" }

    New-Item -Path "$classes\.etxt" -Force | Out-Null
    Set-ItemProperty -Path "$classes\.etxt" -Name '(default)' -Value $progId

    New-Item -Path "$classes\$progId" -Force | Out-Null
    Set-ItemProperty -Path "$classes\$progId" -Name '(default)' -Value '더 나은 메모장 문서'

    New-Item -Path "$classes\$progId\DefaultIcon" -Force | Out-Null
    Set-ItemProperty -Path "$classes\$progId\DefaultIcon" -Name '(default)' -Value "`"$ExePath`",0"

    New-Item -Path "$classes\$progId\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path "$classes\$progId\shell\open\command" -Name '(default)' -Value "`"$ExePath`" `"%1`""

    "registered .etxt -> $ExePath"
}

# 탐색기에 변경을 알린다 (SHCNE_ASSOCCHANGED)
Add-Type -Name Shell -Namespace Reg -MemberDefinition @'
[DllImport("shell32.dll")]
public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);
'@
[Reg.Shell]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
