[CmdletBinding()]
param(
  [string]$StarCraft = (Join-Path ${env:ProgramFiles(x86)} 'StarCraft\x86\StarCraft.exe'),
  [string]$Pluto = 'C:\StarCraft1161\bwapi-data\AI\pluto.dll',
  [ValidateRange(-1,1000)][int]$SpeedMs = 0,
  [switch]$DrawGraph,
  [switch]$Multiplayer,
  [switch]$Challenge,
  [switch]$VerifyOnly,
  # Multiplayer only: opponent account names allowed to face Pluto. Empty = everyone.
  [string[]]$Opponents = @(),
  # Attach to the x86 StarCraft already started by the Battle.net app (keeps its Battle.net login).
  [switch]$Attach,
  # Run in any game type / player mix (UMS, FFA, Top vs Bottom, teams). The 1v1 model treats allies as enemies.
  [switch]$AnyGame,
  # Ignore Pluto's own resignation (default on with -AnyGame: the 1v1 win estimate is unreliable in team games).
  [switch]$NoResign,
  # 'free' pre-seeds the per-opponent opening bandit so Pluto plays without a build hint (fastest/UMS maps).
  [ValidateSet('auto','free')][string]$Opening = 'auto',
  # Show the bridge's Korean notices in the game chat area (off by default; bridge.log always has them).
  [switch]$ShowMessages,
  # Cap the units Pluto observes (nearest own/enemy/neutral first). 0 = default 1700. ~400 halves inference on crowded maps.
  [ValidateRange(0,1700)][int]$MaxUnits = 0
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $projectRoot 'bin'
$runtime = Join-Path $projectRoot $(if ($Challenge) { 'runtime-challenge' } elseif ($Multiplayer) { 'runtime-multiplayer' } else { 'runtime' })
function Assert-Hash([string]$Path, [string]$Expected) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing file: $Path" }
  # Use the built-in .NET implementation, including on Windows PowerShell 5.1
  # installations where the Get-FileHash function is not automatically loaded.
  $algorithm = [System.Security.Cryptography.SHA256]::Create()
  $stream = $null
  try {
    $stream = [System.IO.File]::OpenRead($Path)
    $actual = [System.BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
  } finally {
    if ($null -ne $stream) { $stream.Dispose() }
    $algorithm.Dispose()
  }
  if ($actual -ne $Expected) {
    throw "Unsupported or damaged file: $Path. See README.md for supported versions."
  }
}
$StarCraft = (Resolve-Path -LiteralPath $StarCraft).Path
$Pluto = (Resolve-Path -LiteralPath $Pluto).Path
Assert-Hash $StarCraft '32dbbdd001dd381cb1b3a719b7ad1fc918a9d4bc99661c675e00254efecca827'
Assert-Hash $Pluto '7e360b643c8c0156c03fe0cad9972a3058138ccfe22f921c5b4e0cd0aaf0abef'
$modelDirectory = Join-Path (Split-Path $Pluto -Parent) 'pluto'
Assert-Hash (Join-Path $modelDirectory 'pluto_infer.exe') 'ad880d8be52a6ef03893fa20644b6627e04fcd55e030178c6d52486b82340f2b'
Assert-Hash (Join-Path $modelDirectory 'pluto_weights.bin') '00b400eace6e4782202ebdcb3c30db76054aaa6a08c6a7dcb59575abc2d0a26e'
foreach ($name in @('pluto-scr.dll','scr-loader.exe')) {
  if (-not (Test-Path -LiteralPath (Join-Path $bin $name))) { throw "Missing bin/$name. Run scripts/build.ps1 first." }
}
if ($VerifyOnly) { Write-Output 'Verified StarCraft, Pluto DLL, inference engine, weights and bridge files.'; return }
if (-not $Attach -and (Get-Process -Name StarCraft -ErrorAction SilentlyContinue)) {
  throw 'Exit the existing StarCraft process before starting Pluto (or use -Attach). No existing game was changed.'
}
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
$previousLogs = @('bridge.log','pluto.log','pluto_infer.log') | ForEach-Object { Join-Path $runtime $_ } | Where-Object { Test-Path -LiteralPath $_ }
if ($previousLogs) {
  $archive = Join-Path $runtime ('logs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
  New-Item -ItemType Directory -Path $archive -Force | Out-Null
  foreach ($path in $previousLogs) { Move-Item -LiteralPath $path -Destination $archive }
}
Copy-Item -LiteralPath (Join-Path $bin 'pluto-scr.dll') -Destination $runtime -Force
@('[pluto]',"module=$Pluto","speed_ms=$SpeedMs","multiplayer=$([int]$Multiplayer.IsPresent)","challenge=$([int]$Challenge.IsPresent)","opponents=$($Opponents -join ',')",
  "draw=$([int]$DrawGraph.IsPresent)","any_game=$([int]$AnyGame.IsPresent)","no_resign=$([int]($NoResign.IsPresent -or $AnyGame.IsPresent))","opening=$Opening","show_messages=$([int]$ShowMessages.IsPresent)","max_units=$MaxUnits","straddle=$([int]$Multiplayer.IsPresent)",
  "frame_budget_ms=$(if ($env:BWRL_FRAME_BUDGET_MS) { $env:BWRL_FRAME_BUDGET_MS } elseif ($Multiplayer) { '10' })") | Set-Content -LiteralPath (Join-Path $runtime 'bridge.ini') -Encoding Unicode
$previousDraw = $env:BWRL_DRAW
$previousStraddle = $env:BWRL_STRADDLE
$previousBudget = $env:BWRL_FRAME_BUDGET_MS
try {
  if ($DrawGraph) { $env:BWRL_DRAW = '1' }
  if ($Multiplayer) {
    $env:BWRL_STRADDLE = '1'
    if (-not $env:BWRL_FRAME_BUDGET_MS) { $env:BWRL_FRAME_BUDGET_MS = '10' }
  }
  if ($Attach) {
    $running = @(Get-Process -Name StarCraft -ErrorAction SilentlyContinue)
    $game = $running | Where-Object { $_.Path -eq $StarCraft } | Select-Object -First 1
    if (-not $game) {
      if ($running) { throw "A StarCraft process is running but it is not $StarCraft (64-bit client?). In the Battle.net app: StarCraft > Options > Game Settings > 'Launch 32-bit client', then Play." }
      throw "No running $StarCraft. Start StarCraft from the Battle.net app (32-bit client enabled), log in, then rerun with -Attach."
    }
  } else {
    $game = Start-Process -FilePath $StarCraft -ArgumentList '-launch' -WorkingDirectory $runtime -WindowStyle Hidden -PassThru
  }
} finally {
  $env:BWRL_DRAW = $previousDraw
  $env:BWRL_STRADDLE = $previousStraddle
  $env:BWRL_FRAME_BUDGET_MS = $previousBudget
}
try {
  $ready = $false
  for ($attempt=0; $attempt -lt 60; $attempt++) {
    $game.Refresh()
    if ($game.HasExited) { throw 'StarCraft exited before initialization.' }
    if ($game.MainWindowHandle -ne 0) { $ready=$true; break }
    Start-Sleep -Milliseconds 500
  }
  if (-not $ready) { throw 'StarCraft did not create its window within 30 seconds.' }
  & (Join-Path $bin 'scr-loader.exe') $game.Id (Join-Path $runtime 'pluto-scr.dll')
  if ($LASTEXITCODE -ne 0) { throw 'Bridge loading failed.' }
  $log = Join-Path $runtime 'bridge.log'
  $initialized = $false
  for ($attempt=0; $attempt -lt 30; $attempt++) {
    if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -SimpleMatch '"result":"MH_OK"' -Quiet)) { $initialized=$true; break }
    Start-Sleep -Milliseconds 200
  }
  if (-not $initialized) { throw "Bridge initialization failed. Read $log" }
  if ($Challenge) {
    Write-Output "Experimental 1-v-many bridge ready in StarCraft process $($game.Id). Choose Melee, Free For All or Top vs Bottom."
    Write-Output 'Put Pluto alone on its side, with 1-7 opponents. This uses the original 1v1 model; match quality is unverified.'
  } elseif ($Multiplayer) {
    Write-Output "Experimental multiplayer bridge ready in StarCraft process $($game.Id). Choose a custom 1v1 Melee game."
    Write-Output 'Network synchronization with a second client still requires validation. The game controls multiplayer speed.'
    Write-Output "Allowed opponents: $(if ($Opponents) { $Opponents -join ', ' } else { '(everyone)' })"
    if ($AnyGame) { Write-Output 'Any game type and player mix is admitted (-AnyGame). Allied units are hidden from the 1v1 model; resignation is suppressed.' }
    if ($Opening -eq 'free') { Write-Output 'Opening bandit seeded to the hint-free arm (-Opening free).' }
  } else {
    Write-Output "Pluto ready in StarCraft process $($game.Id). Choose Single Player > Play Custom, Melee, one Computer opponent."
  }
  Write-Output "Logs and persistent opening statistics: $runtime"
} catch {
  # Only this launch's process is owned by the script; never stop an unrelated game.
  $game.Refresh()
  if (-not $game.HasExited) { Stop-Process -Id $game.Id }
  throw
}
