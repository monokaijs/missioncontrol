param([Parameter(Mandatory=$true)][string]$Version)
$ErrorActionPreference = 'Stop'
if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'Invalid version' }
$stage = "build/package/MissionControl-$Version-windows-x64"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item build/Release/MissionControl.exe $stage
Copy-Item README.md,LICENSE,THIRD_PARTY_NOTICES.md $stage
Copy-Item docs "$stage/docs" -Recurse -Force
Copy-Item dist/runtime-imports.txt $stage
$zip = "dist/MissionControl-$Version-windows-x64.zip"
Compress-Archive -Path $stage -DestinationPath $zip -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  MissionControl-$Version-windows-x64.zip" | Set-Content dist/SHA256SUMS.txt -Encoding ascii
Write-Host "Portable artifact: $zip ($((Get-Item $zip).Length) bytes)"
