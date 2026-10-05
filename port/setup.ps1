# Fetch the libraries the loader builds against into port\externals and port\bin.
# Run once before build.bat. Needs git and an internet connection.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$port = $PSScriptRoot
$ext = Join-Path $port 'externals'
$bin = Join-Path $port 'bin'
New-Item -ItemType Directory -Force $ext, $bin | Out-Null

function Invoke-Native {
    & $args[0] $args[1..($args.Count - 1)]
    if ($LASTEXITCODE -ne 0) { throw "$($args -join ' ') failed" }
}

# dynarmic (Azahar fork) at the commit the patch was made against.
$dynarmic = Join-Path $ext 'dynarmic'
if (-not (Test-Path $dynarmic)) {
    Invoke-Native git clone https://github.com/azahar-emu/dynarmic.git $dynarmic
    Invoke-Native git -C $dynarmic checkout a46601580d5512d324104f985b5f0209dc980ddc
    Invoke-Native git -C $dynarmic submodule update --init externals/fmt externals/mcl externals/oaknut externals/robin-map externals/xbyak externals/zycore externals/zydis
    Invoke-Native git -C $dynarmic apply (Join-Path $port 'patches\dynarmic.patch')
}

# Boost 1.86.0 (headers only are used).
if (-not (Test-Path (Join-Path $ext 'boost_1_86_0'))) {
    $archive = Join-Path $ext 'boost.tar.gz'
    Invoke-WebRequest https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz -OutFile $archive
    Invoke-Native tar -xf $archive -C $ext
    Remove-Item $archive
}

# SDL2 2.32.10 development package.
$sdl = Join-Path $ext 'SDL2-2.32.10'
if (-not (Test-Path $sdl)) {
    $archive = Join-Path $ext 'sdl2.zip'
    Invoke-WebRequest https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip -OutFile $archive
    Expand-Archive $archive $ext
    Remove-Item $archive
}
Copy-Item (Join-Path $sdl 'lib\x64\SDL2.dll') $bin -Force

# ANGLE (OpenGL ES on Direct3D). Any Chromium-based app ships it; take it from VS Code or Edge.
$angle = 'libEGL.dll', 'libGLESv2.dll', 'd3dcompiler_47.dll'
if ($angle | Where-Object { -not (Test-Path (Join-Path $bin $_)) }) {
    $roots = "$env:LOCALAPPDATA\Programs\Microsoft VS Code", "$env:ProgramFiles\Microsoft VS Code",
             "${env:ProgramFiles(x86)}\Microsoft\Edge\Application"
    $source = $null
    foreach ($dll in $roots | Where-Object { Test-Path $_ } |
            ForEach-Object { Get-ChildItem $_ -Recurse -Filter libGLESv2.dll -ErrorAction SilentlyContinue }) {
        $dir = $dll.DirectoryName
        if (-not ($angle | Where-Object { -not (Test-Path (Join-Path $dir $_)) })) { $source = $dir; break }
    }
    if (-not $source) { throw "ANGLE DLLs not found. Copy $($angle -join ', ') into $bin by hand." }
    $angle | ForEach-Object { Copy-Item (Join-Path $source $_) $bin -Force }
}

Write-Host 'Externals ready. Now run build.bat.'
