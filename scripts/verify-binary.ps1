param([string]$Executable = 'build/Release/MissionControl.exe')
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$dumpbin = & $vswhere -latest -products '*' -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
if (!$dumpbin) { throw 'dumpbin not found; install Visual Studio C++ build tools' }
$imports = & $dumpbin /DEPENDENTS $Executable
if ($LASTEXITCODE -ne 0) { throw 'Import inspection failed' }
New-Item -ItemType Directory -Force dist | Out-Null
$imports | Out-File dist/runtime-imports.txt -Encoding utf8
$joined = $imports -join "`n"
if ($joined -match '(?i)(VCRUNTIME\d*|MSVCP\d*|MSVCR\d*|ucrtbased)\.dll') { throw 'Unexpected Visual C++ runtime DLL dependency; use static runtime' }
$allowed = @('KERNEL32.dll','USER32.dll','GDI32.dll','ADVAPI32.dll','SHELL32.dll','SHLWAPI.dll','COMCTL32.dll','ole32.dll','OLEAUT32.dll','CRYPT32.dll','bcrypt.dll','WINHTTP.dll','WindowsCodecs.dll','ntdll.dll')
$dlls = [regex]::Matches($joined, '(?im)^\s+([a-z0-9_.-]+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value }
if (!$dlls.Count) { throw 'No imports found' }
foreach ($dll in $dlls) { if ($dll -notin $allowed -and $dll -notmatch '^(?i)api-ms-win-') { throw "Unexpected non-system DLL: $dll" } }
$headers = (& $dumpbin /HEADERS $Executable) -join "`n"
if ($headers -notmatch '8664 machine \(x64\)' -or $headers -notmatch 'Windows GUI') { throw 'Expected x64 GUI subsystem executable' }
$proc = Start-Process -FilePath (Resolve-Path $Executable) -ArgumentList '--self-test' -Wait -PassThru
if ($proc.ExitCode -ne 0) { throw 'Offline executable smoke test failed' }
Write-Host 'Verified x64 GUI executable, system-only imports, static C++ runtime, offline smoke.'
