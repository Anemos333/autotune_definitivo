param(
    [Parameter(Mandatory=$true)][string]$PluginVal,
    [Parameter(Mandatory=$true)][string]$Plugin,
    [string]$ArtifactDirectory = "artifacts/pluginval"
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $ArtifactDirectory | Out-Null
$log = Join-Path $ArtifactDirectory "pluginval-strictness-10.log"

if (-not (Test-Path -LiteralPath $PluginVal)) {
    throw "pluginval executable not found: $PluginVal"
}
if (-not (Test-Path -LiteralPath $Plugin)) {
    throw "VST3 not found: $Plugin"
}

& $PluginVal `
    --strictness-level 10 `
    --validate-in-process `
    --validate $Plugin 2>&1 | Tee-Object -FilePath $log

if ($LASTEXITCODE -ne 0) {
    throw "pluginval failed with exit code $LASTEXITCODE. Log: $log"
}

Write-Host "pluginval strictness 10 passed. Log: $log"
