$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $repo 'build/save-transfer-tests'
New-Item -ItemType Directory -Force $out | Out-Null
& javac -d $out (Join-Path $repo 'android/lasrecomp/app/src/main/java/com/lasrecomp/SaveTransfer.java') (Join-Path $PSScriptRoot 'SaveTransferTest.java')
if ($LASTEXITCODE -ne 0) { throw 'Save transfer test compilation failed.' }
& java "-Djava.io.tmpdir=$out" -cp $out com.lasrecomp.SaveTransferTest
if ($LASTEXITCODE -ne 0) { throw 'Save transfer tests failed.' }
