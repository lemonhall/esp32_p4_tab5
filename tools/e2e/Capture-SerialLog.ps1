param(
  [Parameter(Mandatory = $true)]
  [string]$Port,

  [int]$Baud = 115200,

  [Parameter(Mandatory = $true)]
  [string]$OutFile,

  [int]$OpenTimeoutSec = 30,

  [int]$TimeoutSec = 300,

  # Optional: these patterns indicate "something went wrong". When any is matched we keep capturing for
  # DrainAfterStopMs to include context, then stop.
  #
  # For isolation debugging, pass: -TriggerPatterns @()
  [string[]]$TriggerPatterns = @(
    "assert failed:",
    "Task watchdog got triggered",
    "task_wdt:",
    "Guru Meditation Error",
    "Stack protection fault",
    "Load access fault",
    "Store access fault",
    "panic'ed",
    "abort() was called",
    "mbedtls_ssl_setup returned",
    "mbedtls_ssl_handshake returned",
    "ESP_ERR_MBEDTLS"
  ),

  # Reset detection: ignore the first boot "rst:" line, but stop on the Nth one.
  # This helps avoid false-stops right after flashing (which always prints "rst:" at boot).
  [int]$ResetCountToStop = 2,

  [string]$ResetLinePrefix = "rst:",

  # Capture a little more after a stop pattern is seen (for context).
  [int]$DrainAfterStopMs = 1500,

  # If the serial port disappears (device crash / reset / re-enumeration), try to reopen for
  # PortReopenTimeoutSec. If it still fails and StopOnPortLoss is set, we treat it as a "reset hit"
  # and stop after DrainAfterStopMs.
  [int]$PortReopenTimeoutSec = 20,
  [switch]$StopOnPortLoss = $true,

  # .NET SerialPort defaults can leave DTR/RTS deasserted, which on some ESP32
  # USB-Serial bridges can hold the chip in reset or otherwise suppress output.
  # Make it explicit to match typical terminal behavior.
  [bool]$SetDtrOnOpen = $true,
  [bool]$SetRtsOnOpen = $true
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.IO.Ports

$outDir = Split-Path -Parent $OutFile
if ($outDir -and -not (Test-Path $outDir)) {
  New-Item -ItemType Directory -Path $outDir | Out-Null
}

$writer = [System.IO.StreamWriter]::new($OutFile, $false, [System.Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true

$sp = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.NewLine = "`n"
$sp.ReadTimeout = 200
$sp.Encoding = [System.Text.Encoding]::UTF8

$start = Get-Date
$deadline = $start.AddSeconds($TimeoutSec)
$lastDataAt = $start

try {
  $openDeadline = (Get-Date).AddSeconds($OpenTimeoutSec)
  for (;;) {
    try {
      $sp.Open()
      if ($SetDtrOnOpen) { $sp.DtrEnable = $true }
      if ($SetRtsOnOpen) { $sp.RtsEnable = $true }
      break
    } catch {
      if ((Get-Date) -ge $openDeadline) { throw }
      Start-Sleep -Milliseconds 500
    }
  }

  Write-Host ("[capture] Opened {0} @ {1}" -f $Port, $Baud)
  Write-Host ("[capture] Logging to {0}" -f $OutFile)

  $buf = ""
  $triggerHit = $false
  $triggerAt = $null

  $resetCount = 0
  $resetCountInBufPrev = 0
  $resetHit = $false
  $resetAt = $null
  $portLostHit = $false
  $portLostAt = $null

  while ((Get-Date) -lt $deadline) {
    try {
      $chunk = $sp.ReadExisting()
      if ($chunk) {
        $writer.Write($chunk)
        $buf += $chunk
        $lastDataAt = Get-Date
      }
    } catch {
      # If the device resets, the port can briefly error out. Try reopening.
      if ($sp.IsOpen) {
        try { $sp.Close() } catch {}
      }
      $reopenDeadline = (Get-Date).AddSeconds($PortReopenTimeoutSec)
      $reopened = $false
      while ((Get-Date) -lt $reopenDeadline) {
        try {
          $sp.Open()
          if ($SetDtrOnOpen) { $sp.DtrEnable = $true }
          if ($SetRtsOnOpen) { $sp.RtsEnable = $true }
          $reopened = $true
          break
        } catch {
          Start-Sleep -Milliseconds 200
        }
      }
      if (-not $reopened -and -not $portLostHit) {
        Write-Host ("[capture] Port lost/unavailable (treat as reset). Port={0}" -f $Port)
        $portLostHit = $true
        $portLostAt = Get-Date
      }
    }

    if ($buf.Length -gt 131072) { $buf = $buf.Substring($buf.Length - 131072) }

    if ($ResetLinePrefix) {
      $re = '(?m)^\s*' + [regex]::Escape($ResetLinePrefix)
      $nInBuf = [regex]::Matches($buf, $re).Count
      if ($nInBuf -lt $resetCountInBufPrev) {
        # Buffer was truncated; keep the total count, just reset the "in-buf" baseline.
        $resetCountInBufPrev = $nInBuf
      } elseif ($nInBuf -gt $resetCountInBufPrev) {
        $delta = $nInBuf - $resetCountInBufPrev
        $resetCount += $delta
        $resetCountInBufPrev = $nInBuf
        Write-Host ("[capture] Reset line seen: +{0} total={1}" -f $delta, $resetCount)
      }

      if (-not $resetHit -and $ResetCountToStop -gt 0 -and $resetCount -ge $ResetCountToStop) {
        Write-Host ("[capture] Reset count threshold reached: {0}" -f $ResetCountToStop)
        $resetHit = $true
        $resetAt = Get-Date
      }
    }

    if (-not $triggerHit -and $TriggerPatterns -and $TriggerPatterns.Count -gt 0) {
      foreach ($pat in $TriggerPatterns) {
        if ($pat -and ($buf -like ("*" + $pat + "*"))) {
          Write-Host ("[capture] Trigger pattern matched: {0}" -f $pat)
          $triggerHit = $true
          $triggerAt = Get-Date
          break
        }
      }
    }

    if ($triggerHit) {
      if (((Get-Date) - $triggerAt).TotalMilliseconds -ge $DrainAfterStopMs) { break }
    }
    elseif ($resetHit) {
      if (((Get-Date) - $resetAt).TotalMilliseconds -ge $DrainAfterStopMs) { break }
    }
    elseif ($portLostHit -and $StopOnPortLoss) {
      if (((Get-Date) - $portLostAt).TotalMilliseconds -ge $DrainAfterStopMs) { break }
    }

    Start-Sleep -Milliseconds 50
  }

  if ($triggerHit) {
    Write-Host "[capture] STOPPED (trigger)"
    exit 0
  }
  if ($resetHit) {
    Write-Host "[capture] STOPPED (reset-threshold)"
    exit 0
  }
  if ($portLostHit -and $StopOnPortLoss) {
    Write-Host "[capture] STOPPED (port-loss)"
    exit 0
  }

  Write-Host "[capture] STOPPED (timeout)"
  exit 0
}
finally {
  try { if ($sp.IsOpen) { $sp.Close() } } catch {}
  try { $writer.Dispose() } catch {}
}
