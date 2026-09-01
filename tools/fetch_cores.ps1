# Downloads the game cores and their support/content files into the
# repository's cores/ directory (build inputs, not committed to the repo):
#  - libretro cores from the official buildbot (mrboom, snes9x, prboom)
#  - prboom.wad, the prboom core's resource wad (libretro-prboom repo, GPL-2)
#  - freedoom1.wad, the freely redistributable Doom IWAD (Freedoom project)
#
# Usage: fetch_cores.ps1 [-Cores mrboom,snes9x,prboom]

param([string[]]$Cores = @("mrboom", "snes9x", "prboom"))

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$coresDir = Join-Path $repoRoot "cores"
New-Item -ItemType Directory -Force $coresDir | Out-Null

foreach ($core in $Cores) {
    $zipPath = Join-Path $coresDir "${core}_libretro.dll.zip"
    $url = "https://buildbot.libretro.com/nightly/windows/x86_64/latest/${core}_libretro.dll.zip"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zipPath
    Expand-Archive -Force $zipPath $coresDir
    Remove-Item $zipPath
    Write-Host "Core ready: $(Join-Path $coresDir "${core}_libretro.dll")"
}

if ($Cores -contains "prboom") {
    $prboomWad = Join-Path $coresDir "prboom.wad"
    Write-Host "Downloading prboom.wad (prboom core resource wad)"
    Invoke-WebRequest -Uri "https://github.com/libretro/libretro-prboom/raw/master/prboom.wad" -OutFile $prboomWad

    $freedoomVersion = "0.13.0"
    $freedoomZip = Join-Path $coresDir "freedoom.zip"
    Write-Host "Downloading Freedoom $freedoomVersion (freedoom1.wad, the default Doom IWAD)"
    Invoke-WebRequest -Uri "https://github.com/freedoom/freedoom/releases/download/v$freedoomVersion/freedoom-$freedoomVersion.zip" -OutFile $freedoomZip
    Expand-Archive -Force $freedoomZip $coresDir
    Copy-Item (Join-Path $coresDir "freedoom-$freedoomVersion\freedoom1.wad") (Join-Path $coresDir "freedoom1.wad") -Force
    Remove-Item -Recurse -Force (Join-Path $coresDir "freedoom-$freedoomVersion"), $freedoomZip
    Write-Host "Doom content ready: prboom.wad + freedoom1.wad"
}
