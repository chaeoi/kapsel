param([Parameter(Mandatory)][string]$Root)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $Root).Path
$qemu = Join-Path $Root 'runtime/qemu-system-x86_64.exe'
$imageTool = Join-Path $Root 'runtime/qemu-img.exe'
foreach ($program in @($qemu, $imageTool)) {
    & $program --version
    if ($LASTEXITCODE -ne 0) { throw "Failed to load $program" }
}

$app = Start-Process -FilePath (Join-Path $Root 'Kapsel.exe') -WorkingDirectory $Root -PassThru
$log = Join-Path $Root 'data/qemu.log'
try {
    $deadline = (Get-Date).AddSeconds(45)
    $disk = Join-Path $Root 'data/debian.qcow2'
    $vm = $null
    while ((Get-Date) -lt $deadline) {
        $app.Refresh()
        if ($app.HasExited) { throw 'Kapsel exited during startup.' }
        $vm = Get-Process 'qemu-system-x86_64' -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $qemu } | Select-Object -First 1
        if ($vm -and (Test-Path $disk)) { break }
        Start-Sleep -Milliseconds 200
    }
    if (-not $vm -or -not (Test-Path $disk)) { throw 'Debian VM did not start.' }
    Start-Sleep -Seconds 4
    $vm.Refresh()
    if ($vm.HasExited) { throw 'QEMU exited during guest startup.' }
    Write-Host "Kapsel started QEMU and opened the portable disk."
} finally {
    if (Test-Path $log) { Get-Content $log }
    if ($vm -and -not $vm.HasExited) { Stop-Process -Id $vm.Id -Force }
    if (-not $app.HasExited) { Stop-Process -Id $app.Id -Force }
}
