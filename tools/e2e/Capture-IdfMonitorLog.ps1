param(
  [Parameter(Mandatory = $true)]
  [string]$Port,

  [int]$Baud = 115200,

  [Parameter(Mandatory = $true)]
  [string]$OutFile,

  [Parameter(Mandatory = $true)]
  [string]$ElfPath,

  # Optional: if IDF_PATH isn't set, source this profile to populate ESP-IDF env.
  [string]$IdfProfile = "D:\\Espressif\\tools\\Microsoft.v5.4.2.PowerShell_profile.ps1",

  [int]$TimeoutSec = 300,

  [string[]]$TriggerPatterns = @(
    "assert failed:",
    "abort() was called",
    "Task watchdog got triggered",
    "task_wdt:",
    "Guru Meditation Error",
    "Stack protection fault",
    "Load access fault",
    "Store access fault",
    "panic'ed",
    "websocket init failed",
    "Memory exhausted",
    "Invalid uri",
    "Failed to set the configuration",
    "Fatal error: TLSP deletion callback",
    "mbedtls_ssl_setup returned",
    "mbedtls_ssl_handshake returned",
    "ESP_ERR_MBEDTLS"
  ),

  [string[]]$ArmAfterPatterns = @(
    "boot: Loaded app from partition",
    "app_init: Application information:"
  ),

  [string]$ResetLinePrefix = "rst:",

  # Optional: stop after N total resets (including the initial boot `rst:` line).
  # Useful for "flash -> boot -> crash -> reboot" capture loops (set to 2).
  [int]$ResetCountToStopTotal = 0,

  [int]$ResetCountToStopAfterArm = 1,

  [int]$DrainAfterStopMs = 1500
)

$ErrorActionPreference = "Stop"

function Ensure-Dir([string]$path) {
  if (-not $path) { return }
  if (-not (Test-Path $path)) { New-Item -ItemType Directory -Path $path | Out-Null }
}

function Match-Any([string]$line, [string[]]$patterns) {
  if (-not $line) { return $null }
  if (-not $patterns) { return $null }
  foreach ($p in $patterns) {
    if ($p -and ($line -like ("*" + $p + "*"))) { return $p }
  }
  return $null
}

function Stop-MonitorsOnPort([string]$port) {
  if (-not $port) { return }
  try {
    $pids = Get-CimInstance Win32_Process |
      Where-Object {
        $_.CommandLine -and
        ($_.CommandLine -match [regex]::Escape($port)) -and
        ($_.CommandLine -match '(idf_monitor\\.py|esp_idf_monitor)')
      } |
      Select-Object -ExpandProperty ProcessId
    foreach ($id in ($pids | Select-Object -Unique)) {
      try { Stop-Process -Id $id -Force -ErrorAction Stop } catch {}
    }
  } catch {
    # best-effort
  }
}

$outDir = Split-Path -Parent $OutFile
Ensure-Dir $outDir

$idfPath = $env:IDF_PATH
if (-not $idfPath -and $IdfProfile -and (Test-Path $IdfProfile)) {
  . $IdfProfile
  $idfPath = $env:IDF_PATH
}
if (-not $idfPath) {
  throw "IDF_PATH is not set. Did you source the ESP-IDF PowerShell profile first?"
}
$mon = Join-Path $idfPath "tools\\idf_monitor.py"
if (-not (Test-Path $mon)) { throw ("Missing idf_monitor.py: {0}" -f $mon) }
if (-not (Test-Path $ElfPath)) { throw ("Missing ELF: {0}" -f $ElfPath) }

# Start monitor process, stream stdout/stderr ourselves (avoid Start-Process redirection buffering).
Write-Host ("[monitor] Logging to {0}" -f $OutFile)

$outDir2 = Split-Path -Parent $OutFile
Ensure-Dir $outDir2

$writer = [System.IO.StreamWriter]::new($OutFile, $false, [System.Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true

$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = "python"
foreach ($a in @(
  "-u",
  $mon,
  "-p", $Port,
  "-b", $Baud,
  "--toolchain-prefix", "riscv32-esp-elf-",
  "--target", "esp32p4",
  "--decode-panic", "backtrace",
  $ElfPath
)) {
  $null = $psi.ArgumentList.Add([string]$a)
}
$psi.WorkingDirectory = (Get-Location).Path
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::new()
$proc.StartInfo = $psi
if (-not $proc.Start()) {
  throw "Failed to start idf_monitor.py"
}

$start = Get-Date
$deadline = $start.AddSeconds($TimeoutSec)

$armed = $false
$resetAfterArm = 0
$resetTotal = 0
$downloadHintPrinted = $false

$stopKind = $null
$stopWhy = $null
$stopAt = $null

$stdoutTask = $proc.StandardOutput.ReadLineAsync()
$stderrTask = $proc.StandardError.ReadLineAsync()

try {
  while ((Get-Date) -lt $deadline) {
    if ($stdoutTask -and $stdoutTask.Wait(10)) {
      $line = $stdoutTask.Result
      $stdoutTask = $proc.StandardOutput.ReadLineAsync()
      if ($null -ne $line) {
        $writer.WriteLine($line)
        $writer.Flush()
        $line = $line.TrimEnd("`r")
        if (-not $downloadHintPrinted -and -not $armed) {
          if ($line -like "*waiting for download*" -or $line -like "*DOWNLOAD(USB/UART0/SPI)*") {
            Write-Host "[monitor] Device is in DOWNLOAD mode. Short-press Reset once to boot the app."
            $downloadHintPrinted = $true
          }
        }
        if (-not $armed) {
          $armBy = Match-Any $line $ArmAfterPatterns
          if ($armBy) {
            Write-Host ("[monitor] ARMED by: {0}" -f $armBy)
            $armed = $true
          }
        }
        if (-not $stopKind) {
          $hit = Match-Any $line $TriggerPatterns
          if ($hit) {
            Write-Host ("[monitor] Trigger matched: {0}" -f $hit)
            $stopKind = "trigger"
            $stopWhy = $hit
            $stopAt = Get-Date
          } elseif ($ResetLinePrefix) {
            $re = '^\s*' + [regex]::Escape($ResetLinePrefix)
            if ($line -match $re) {
              $resetTotal++
              if (-not $armed) {
                Write-Host ("[monitor] Reset total (unarmed): {0}" -f $resetTotal)
              } else {
                $resetAfterArm++
                Write-Host ("[monitor] Reset after arm: {0}/{1} (total={2})" -f $resetAfterArm, $ResetCountToStopAfterArm, $resetTotal)
              }

              if ($ResetCountToStopTotal -gt 0 -and $resetTotal -ge $ResetCountToStopTotal) {
                $stopKind = "reset-total"
                $stopWhy = ("resetTotal={0}" -f $resetTotal)
                $stopAt = Get-Date
              } elseif ($armed -and $ResetCountToStopAfterArm -gt 0 -and $resetAfterArm -ge $ResetCountToStopAfterArm) {
                $stopKind = "reset-after-arm"
                $stopWhy = ("resetAfterArm={0}" -f $resetAfterArm)
                $stopAt = Get-Date
              }
            }
          }
        }
      }
    }
    if ($stderrTask -and $stderrTask.Wait(10)) {
      $line = $stderrTask.Result
      $stderrTask = $proc.StandardError.ReadLineAsync()
      if ($null -ne $line) {
        $writer.WriteLine($line)
        $writer.Flush()
      }
    }

    if ($stopKind -and $stopAt) {
      if (((Get-Date) - $stopAt).TotalMilliseconds -ge $DrainAfterStopMs) { break }
    }

    if ($proc.HasExited) { break }
    Start-Sleep -Milliseconds 20
  }
}
finally {
  try { $writer.Dispose() } catch {}
  Stop-MonitorsOnPort $Port
  if ($proc -and -not $proc.HasExited) {
    try { taskkill.exe /PID $proc.Id /T /F 2>$null | Out-Null } catch {}
    try { $proc.Kill() } catch {}
  }
}

if ($stopKind) {
  Write-Host ("[monitor] STOPPED ({0}) {1}" -f $stopKind, ($stopWhy ? ("why=" + $stopWhy) : ""))
  exit 0
}

if ((Get-Date) -ge $deadline) {
  Write-Host "[monitor] STOPPED (timeout)"
  exit 0
}

Write-Host "[monitor] STOPPED (monitor-exited)"
exit 0
