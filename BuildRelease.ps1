param(
    [string]$preset = "release",
    [int]$threads = 8
)


Write-Host "Running preset $preset"

# Save current directory, launch VS dev shell, and return to original directory
$vsDevShellPath = "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/Tools/Launch-VsDevShell.ps1"
$currentDirectory = $PWD.Path
& $vsDevShellPath -Arch amd64; Set-Location -Path "${currentDirectory}"

# Build cmake configure arguments
$cmakeArgs = @("-S", ".", "--preset=$preset", "-DCMAKE_COMPILE_JOBS=$threads", "-Wno-dev")

& cmake $cmakeArgs
if ($LASTEXITCODE -ne 0) { exit 1 }

& cmake --build --preset=$preset --parallel $threads
if ($LASTEXITCODE -ne 0) { exit 1 }