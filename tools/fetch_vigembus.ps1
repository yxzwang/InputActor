param(
    [string]$OutputPath = "vendor\ViGEmBusSetup.exe",
    [switch]$ForceDownload
)

$ErrorActionPreference = "Stop"

$outputFullPath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputPath))
$outputDir = Split-Path -Path $outputFullPath -Parent
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

if ((Test-Path $outputFullPath) -and (-not $ForceDownload)) {
    Write-Host "Using existing ViGEmBus installer: $outputFullPath"
    exit 0
}

$headers = @{
    "Accept" = "application/vnd.github+json"
    "User-Agent" = "InputActor-Build"
}

$release = Invoke-RestMethod `
    -Uri "https://api.github.com/repos/ViGEm/ViGEmBus/releases/latest" `
    -Headers $headers

$asset = $release.assets `
    | Where-Object { $_.name -match "^ViGEmBus_.*\.exe$" } `
    | Select-Object -First 1

if (-not $asset) {
    throw "Cannot find ViGEmBus installer asset in latest release."
}

Write-Host "Downloading ViGEmBus from: $($asset.browser_download_url)"
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $outputFullPath

$hash = (Get-FileHash -Path $outputFullPath -Algorithm SHA256).Hash
$metadata = [ordered]@{
    tag_name       = $release.tag_name
    asset_name     = $asset.name
    download_url   = $asset.browser_download_url
    sha256         = $hash
    downloaded_utc = [DateTime]::UtcNow.ToString("o")
}

$metadataPath = [System.IO.Path]::ChangeExtension($outputFullPath, ".json")
$metadata | ConvertTo-Json -Depth 4 | Set-Content -Path $metadataPath -Encoding UTF8

Write-Host "ViGEmBus installer saved to: $outputFullPath"
Write-Host "SHA256: $hash"
