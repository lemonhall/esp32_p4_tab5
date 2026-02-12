param(
  [Parameter(Mandatory = $true)]
  [string]$Port,

  [int]$Baud = 115200,

  [int]$TimeoutSec = 180,

  [string[]]$Patterns = @(
    "sta got ip",
    "connected irc.lemonhall.me:6667",
    "join #tab5"
  )
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.IO.Ports

$sp = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.NewLine = "`n"
$sp.ReadTimeout = 200

$start = Get-Date
$deadline = $start.AddSeconds($TimeoutSec)

try {
  $sp.Open()
  Write-Host ("[e2e] Opened {0} @ {1}" -f $Port, $Baud)

  $pending = [System.Collections.Generic.List[string]]::new()
  foreach ($p in $Patterns) { $null = $pending.Add($p) }

  $buf = ""
  while ((Get-Date) -lt $deadline) {
    try {
      $chunk = $sp.ReadExisting()
      if ($chunk) { $buf += $chunk }
    } catch {
      # ignore timeouts
    }

    if ($buf.Length -gt 65536) { $buf = $buf.Substring($buf.Length - 65536) }

    for ($i = $pending.Count - 1; $i -ge 0; $i--) {
      $pat = $pending[$i]
      if ($buf -like ("*" + $pat + "*")) {
        Write-Host ("[e2e] Matched: {0}" -f $pat)
        $pending.RemoveAt($i)
      }
    }

    if ($pending.Count -eq 0) {
      Write-Host "[e2e] PASS"
      exit 0
    }

    Start-Sleep -Milliseconds 100
  }

  Write-Host ("[e2e] FAIL. Missing patterns: {0}" -f ($pending -join ", "))
  exit 1
}
finally {
  if ($sp.IsOpen) { $sp.Close() }
}

