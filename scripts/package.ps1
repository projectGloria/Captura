param([string]$GloriaSetup = (Join-Path $PSScriptRoot '..\..\GloriaSetup'), [switch]$StageOnly)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$cache = Join-Path $root 'target\deps'
$stage = Join-Path $root ('target\package\' + [guid]::NewGuid().ToString('N'))
$ocr = Join-Path $stage 'ocr'
$licenses = Join-Path $ocr 'licenses'
New-Item -ItemType Directory -Force $cache,$licenses | Out-Null

function Get-VerifiedFile($Url, $File, $Expected) {
    if (!(Test-Path -LiteralPath $File) -or (Get-FileHash -LiteralPath $File).Hash -ne $Expected) {
        Invoke-WebRequest -Uri $Url -OutFile $File
    }
    if ((Get-FileHash -LiteralPath $File).Hash -ne $Expected) { throw "Checksum mismatch for $File" }
}

Push-Location $root
try {
    cargo build --locked --release
    if ($LASTEXITCODE) { throw 'Release build failed.' }
    Copy-Item target\release\Captura.exe,Captura.ico -Destination $stage
    $nuget = Join-Path $cache 'tesseract.zip'
    Get-VerifiedFile 'https://api.nuget.org/v3-flatcontainer/tesseract/5.2.0/tesseract.5.2.0.nupkg' $nuget '202D82FC7C7D8384DF7DA57206D5E1F456CCDABD648C46E67CDFAA3A911D4795'
    $model = Join-Path $cache 'eng.traineddata'
    Get-VerifiedFile 'https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/87416418657359cb625c412a48b6e1d6d41c29bd/eng.traineddata' $model '7D4322BD2A7749724879683FC3912CB542F19906C83BCC1A52132556427170B2'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($nuget)
    try {
        foreach ($dll in 'tesseract50.dll','leptonica-1.82.0.dll') {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($zip.GetEntry("x64/$dll"), (Join-Path $ocr $dll))
        }
    } finally { $zip.Dispose() }
    Copy-Item $model $ocr
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (!$crt) { throw 'Install the x64 MSVC redistributable build tools.' }
    foreach ($dll in 'msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll') {
        Copy-Item (Join-Path $crt.FullName $dll) $ocr
    }
    Copy-Item (Join-Path $root 'licenses\*') $licenses
    # Keep the development release directly runnable with the same OCR assets.
    Copy-Item $ocr (Join-Path $root 'target\release') -Recurse -Force
    if ($StageOnly) {
        if ($env:GITHUB_OUTPUT) { "app-dir=$stage" >> $env:GITHUB_OUTPUT }
        Write-Host "Staged Windows x64 payload at $stage"
        return
    }
    $GloriaSetup = (Resolve-Path $GloriaSetup).Path
    $pack = Join-Path $GloriaSetup 'target\release\gloria-pack.exe'
    $stub = Join-Path $GloriaSetup 'target\release\gloria-stub.exe'
    if (!(Test-Path $pack) -or !(Test-Path $stub)) {
        cargo build --release --manifest-path (Join-Path $GloriaSetup 'Cargo.toml') -p gloria-pack -p gloria-stub
        if ($LASTEXITCODE) { throw 'GloriaSetup build failed.' }
    }
    & $pack --app $stage --manifest (Join-Path $root 'gloria.toml') --stub $stub --out (Join-Path $root 'dist')
    if ($LASTEXITCODE) { throw 'GloriaSetup packaging failed.' }
} finally { Pop-Location }
