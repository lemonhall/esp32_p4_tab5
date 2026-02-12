param(
  [string]$Port = "COM6",
  [int]$FlashBaud = 460800,
  [int]$MonitorBaud = 115200,
  [int]$CaptureSec = 300,
  [string]$ProjectDir = "M5Tab5-UserDemo\\platforms\\tab5",
  [string]$IdfProfile = "D:\\Espressif\\tools\\Microsoft.v5.4.2.PowerShell_profile.ps1",
  [switch]$DecodeAddrs = $true,

  # Isolation mode: do "reset counter only" capture.
  # - ignore first "rst:" (normal boot)
  # - stop on second "rst:" (indicates reboot after crash/panic)
  # No other trigger keywords are used.
  [switch]$IsolationRstOnly = $true
)

$ErrorActionPreference = "Stop"

function Assert-FileExists([string]$path) {
  if (-not (Test-Path $path)) {
    throw ("Missing: {0}" -f $path)
  }
}

function Stop-Tab5PortMonitors([string]$port) {
  if (-not $port) { return }
  try {
    $procs = Get-CimInstance Win32_Process |
      Where-Object {
        $_.CommandLine -and
        ($_.CommandLine -match [regex]::Escape($port)) -and
        ($_.CommandLine -match '(idf_monitor\\.py|esp_idf_monitor|idf\\.py\\s+-p\\s+\\w+\\s+monitor)')
      } |
      Select-Object -ExpandProperty ProcessId

    if ($procs) {
      Write-Host ("[greenflash] stopping stale monitor processes on {0}: {1}" -f $port, ($procs -join ", "))
      foreach ($pid in ($procs | Select-Object -Unique)) {
        try { Stop-Process -Id $pid -Force -ErrorAction Stop } catch {}
      }
    }
  } catch {
    # best-effort
  }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$projPath = Join-Path $repoRoot $ProjectDir
$buildPath = Join-Path $projPath "build"
$elfPath = Join-Path $buildPath "m5stack_tab5.elf"

Assert-FileExists $IdfProfile
Assert-FileExists (Join-Path $projPath "CMakeLists.txt")
Assert-FileExists (Join-Path $repoRoot "tools\\e2e\\Capture-IdfMonitorLog.ps1")

. $IdfProfile

Write-Host ("[greenflash] repo={0}" -f $repoRoot)
Write-Host ("[greenflash] project={0}" -f $projPath)
Write-Host ("[greenflash] port={0}" -f $Port)

Stop-Tab5PortMonitors $Port

Push-Location $projPath
try {
  Write-Host "[greenflash] idf.py build"
  idf.py build
  if ($LASTEXITCODE -ne 0) { throw ("idf.py build failed (exit={0})" -f $LASTEXITCODE) }

  if (-not (Test-Path $buildPath)) { throw "build dir missing after build" }
  if (-not (Test-Path $elfPath)) { throw "ELF missing after build" }

  $ts = Get-Date -Format "yyyyMMdd-HHmmss"
  $logDir = Join-Path $repoRoot "tools\\logs"
  $logPath = Join-Path $logDir ("tab5-{0}-{1}.log" -f $Port, $ts)

  Write-Host ("[greenflash] FLASH (expects device already in download mode: 绿闪了)" )
  Push-Location $buildPath
  try {
    python -m esptool --chip esp32p4 -p $Port -b $FlashBaud --before no_reset --after hard_reset write_flash "@flash_args"
    if ($LASTEXITCODE -ne 0) {
      Write-Host "[greenflash] write_flash failed with --before no_reset; retry with --before default_reset"
      python -m esptool --chip esp32p4 -p $Port -b $FlashBaud --before default_reset --after hard_reset write_flash "@flash_args"
    }
    if ($LASTEXITCODE -ne 0) { throw ("esptool write_flash failed (exit={0}). Is {1} busy?" -f $LASTEXITCODE, $Port) }
  }
  finally {
    Pop-Location
  }

  Write-Host "[greenflash] Flash done. If the device stays in DOWNLOAD mode, short-press Reset once to boot the app."

  Write-Host "[greenflash] CAPTURE idf.py monitor log"
  $capParams = @{
    Port            = $Port
    Baud            = $MonitorBaud
    OutFile         = $logPath
    ElfPath         = $elfPath
    TimeoutSec      = $CaptureSec
    ResetCountToStopAfterArm = 1
  }
  if ($IsolationRstOnly) {
    $capParams["TriggerPatterns"] = @()
    $capParams["ResetCountToStopTotal"] = 2
  }
  & (Join-Path $repoRoot "tools\\e2e\\Capture-IdfMonitorLog.ps1") @capParams

  Write-Host ("[greenflash] log saved: {0}" -f $logPath)
  Write-Host "[greenflash] quick scan:"
  Select-String -Path $logPath -Pattern "assert failed:","abort() was called","Task watchdog got triggered","task_wdt:","Guru Meditation Error","Stack protection fault","Load access fault","Store access fault","panic'ed","Fatal error: TLSP deletion callback","mbedtls_ssl_setup returned","mbedtls_ssl_handshake returned","ESP_ERR_MBEDTLS","heap@","heap_integrity@","Rebooting...","rst:" -SimpleMatch -AllMatches |
    Select-Object -First 80 |
    ForEach-Object { "[hit] {0}:{1} {2}" -f $_.Filename, $_.LineNumber, $_.Line.TrimEnd() } |
    Write-Host

  if ($DecodeAddrs) {
    $addr2line = "riscv32-esp-elf-addr2line"
    $hasAddr2line = Get-Command $addr2line -ErrorAction SilentlyContinue
    if ($hasAddr2line) {
      Write-Host "[greenflash] addr2line decode (best-effort):"

      $raw = Get-Content -Raw -Path $logPath
      if (-not $raw) {
        Write-Host "[greenflash] empty log (skip decode)"
        $raw = ""
      }
      $addrs = New-Object System.Collections.Generic.List[string]

      foreach ($m in [regex]::Matches($raw, "MEPC\\s*:\\s*(0x[0-9a-fA-F]+)")) { $null = $addrs.Add($m.Groups[1].Value) }
      foreach ($m in [regex]::Matches($raw, "\\bRA\\s*:\\s*(0x[0-9a-fA-F]+)")) { $null = $addrs.Add($m.Groups[1].Value) }
      foreach ($m in [regex]::Matches($raw, "\\bPC\\s*:\\s*(0x[0-9a-fA-F]+)")) { $null = $addrs.Add($m.Groups[1].Value) }
      foreach ($m in [regex]::Matches($raw, "\\bCore\\d+ Saved PC\\s*:\\s*(0x[0-9a-fA-F]+)")) { $null = $addrs.Add($m.Groups[1].Value) }

      foreach ($m in [regex]::Matches($raw, "^Backtrace:.*$", [System.Text.RegularExpressions.RegexOptions]::Multiline)) {
        foreach ($a in [regex]::Matches($m.Value, "0x[0-9a-fA-F]+")) { $null = $addrs.Add($a.Value) }
      }

      $uniq = $addrs | Select-Object -Unique | Select-Object -First 80
      foreach ($a in $uniq) {
        try {
          $out = & $addr2line -pfiaC -e $elfPath $a 2>$null
          if ($out) { Write-Host ("[a2l] {0} -> {1}" -f $a, ($out -join " ")) }
        } catch {
          # ignore
        }
      }
    } else {
      Write-Host "[greenflash] addr2line not found in PATH (skip decode)"
    }
  }

  Write-Host "[greenflash] tail:"
  Get-Content -Path $logPath -Tail 120 | ForEach-Object { $_ } | Write-Host
}
finally {
  Pop-Location
}
