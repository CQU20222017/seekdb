<#
.SYNOPSIS
    One-shot end-to-end verification of competition task 1 (AI function
    enhancement) on a Windows build of seekdb.

.DESCRIPTION
    Runs, in order:

      1. probe        - print the seekdb binary's --version / --help
      2. seekdb       - start a local instance unless the port is already busy
      3. mock         - start the deterministic AI mock on port 8000
      4. database     - CREATE DATABASE IF NOT EXISTS <db>
      5. regression   - run the ai_function mysqltest cases against .result
      6. mock events  - summarise which mock endpoints were hit
      7. summary      - PASS/FAIL per step and overall

    Every step prints "[verify][ok|FAIL] <step>".  The script stops at the first
    failing step and names it, and it never starts a second instance when the
    seekdb port (or the mock port) is already listening.

.EXAMPLE
    .\tools\competition\verify.ps1
    .\tools\competition\verify.ps1 -Port 2881 -All -StopSeekdb
    .\tools\competition\verify.ps1 -Binary D:\seekdb-2026\build_release\src\observer\seekdb.exe

.EXAMPLE
    # instance already running and you only want the regression part
    .\tools\competition\verify.ps1 -SkipProbe -KeepSeekdb
#>
[CmdletBinding()]
param(
    # MySQL port of the seekdb instance
    [int]$Port = 2881,
    # port of the AI mock service (the .test files hardcode 8000)
    [int]$MockPort = 8000,
    # seekdb binary; relative paths resolve against the repository root
    [string]$Binary = "build_release/src/observer/seekdb.exe",
    # base dir for the throwaway instance; default: %TEMP%\seekdb_verify_<port>
    [string]$BaseDir = "",
    # connection parameters of the instance
    [string]$User = "root",
    [string]$Password = "",
    [string]$Database = "test",
    # run the two DDL cases as well
    [switch]$All,
    # do not touch an already running instance at the end
    [switch]$KeepSeekdb,
    # stop the seekdb instance in the final step (only if this script started it)
    [switch]$StopSeekdb,
    # keep the mock service running in the end
    [switch]$KeepMock,
    # skip step 1
    [switch]$SkipProbe,
    # python interpreter that has pymysql installed
    [string]$Python = $(if ($env:SEEKDB_PYTHON) { $env:SEEKDB_PYTHON } else { "python" }),
    # extra arguments forwarded to run_ai_function_tests.py
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = "Continue"
$script:Results = New-Object System.Collections.ArrayList
$script:Python = $Python
$script:Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script:Tools = Join-Path $script:Root "tools/competition"
$script:StartedSeekdb = $false
$script:StartedMock = $false

function Write-Step {
    param([string]$Name)
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor DarkGray
    Write-Host ("== " + $Name) -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkGray
}

function Write-Ok {
    param([string]$Name, [string]$Detail = "")
    $script:Results.Add([pscustomobject]@{ Step = $Name; Status = "ok"; Detail = $Detail }) | Out-Null
    Write-Host ("[verify][ok]   " + $Name + $(if ($Detail) { " - " + $Detail } else { "" })) -ForegroundColor Green
}

function Write-Fail {
    param([string]$Name, [string]$Detail = "")
    $script:Results.Add([pscustomobject]@{ Step = $Name; Status = "FAIL"; Detail = $Detail }) | Out-Null
    Write-Host ("[verify][FAIL] " + $Name + $(if ($Detail) { " - " + $Detail } else { "" })) -ForegroundColor Red
    Write-Host ""
    Write-Host "verification aborted at step: $Name" -ForegroundColor Red
    if ($Detail) { Write-Host "reason: $Detail" -ForegroundColor Red }
    Show-Summary
    exit 1
}

function Show-Summary {
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor DarkGray
    Write-Host "== summary" -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkGray
    $failed = 0
    foreach ($row in $script:Results) {
        if ($row.Status -eq "ok") {
            Write-Host ("  {0,-28} {1}" -f $row.Step, "ok") -ForegroundColor Green
        }
        else {
            Write-Host ("  {0,-28} {1}" -f $row.Step, "FAIL") -ForegroundColor Red
            $failed++
        }
    }
    if ($failed -gt 0) {
        Write-Host ""
        Write-Host "OVERALL: FAIL ($failed step(s))" -ForegroundColor Red
    }
    else {
        Write-Host ""
        Write-Host "OVERALL: PASS" -ForegroundColor Green
    }
}

function Test-PortOpen {
    param([int]$PortToCheck)
    & $script:Python -c "import socket,sys; s=socket.socket(); s.settimeout(0.4); sys.exit(0 if s.connect_ex(('127.0.0.1',$PortToCheck))==0 else 1)" | Out-Null
    return ($LASTEXITCODE -eq 0)
}

function Invoke-Tool {
    <# runs a python tool, streams its output to the console, returns the exit code #>
    param([string[]]$ToolArgs)
    & $script:Python @ToolArgs | Out-Host
    return $LASTEXITCODE
}

$sep = [System.IO.Path]::DirectorySeparatorChar
Write-Host "seekdb task-1 verification" -ForegroundColor White
Write-Host "  repo      : $script:Root"
Write-Host "  binary    : $Binary"
Write-Host "  seekdb    : 127.0.0.1:$Port (user=$User, db=$Database)"
Write-Host "  mock      : 127.0.0.1:$MockPort"
Write-Host "  python    : $($script:Python)"

# ---------------------------------------------------------------------------
# step 1: probe the binary
# ---------------------------------------------------------------------------
if (-not $SkipProbe) {
    Write-Step "1/7 probe the seekdb binary"
    $code = Invoke-Tool @("$script:Tools/seekdb_server.py", "probe", "--binary", $Binary)
    if ($code -ne 0) {
        Write-Fail "probe" "the binary is missing or does not answer -h/-V (exit $code)"
    }
    Write-Ok "probe" "binary answered -h and -V"
}
else {
    Write-Host "[verify][skip] probe (--SkipProbe)" -ForegroundColor DarkGray
    $script:Results.Add([pscustomobject]@{ Step = "probe"; Status = "ok"; Detail = "skipped" }) | Out-Null
}

# ---------------------------------------------------------------------------
# step 2: seekdb instance
# ---------------------------------------------------------------------------
Write-Step "2/7 seekdb instance on port $Port"
if (Test-PortOpen -PortToCheck $Port) {
    Write-Host "[verify] port $Port already listening - reusing that instance" -ForegroundColor Yellow
    Write-Ok "seekdb" "already running on port $Port (not started by this script)"
}
else {
    if (-not $BaseDir) {
        $BaseDir = Join-Path $env:TEMP ("seekdb_verify_" + $Port)
    }
    Write-Host "[verify] base dir: $BaseDir"
    $code = Invoke-Tool @("$script:Tools/seekdb_server.py", "start",
        "--binary", $Binary, "--port", "$Port", "--base-dir", $BaseDir,
        "--user", $User)
    if ($code -ne 0) {
        Write-Fail "seekdb start" "seekdb did not become ready on port $Port (exit $code); see the log tail above"
    }
    $script:StartedSeekdb = $true
    Write-Ok "seekdb" "started on port $Port (base dir $BaseDir)"
}

# ---------------------------------------------------------------------------
# step 3: mock AI service
# ---------------------------------------------------------------------------
Write-Step "3/7 deterministic AI mock on port $MockPort"
if (Test-PortOpen -PortToCheck $MockPort) {
    Write-Host "[verify] port $MockPort already listening - reusing that service" -ForegroundColor Yellow
    Write-Ok "mock start" "already running on port $MockPort (not started by this script)"
}
else {
    $code = Invoke-Tool @("$script:Tools/ai_function_mock.py", "start", "$MockPort")
    if ($code -ne 0) {
        Write-Fail "mock start" "the mock service did not start on port $MockPort (exit $code)"
    }
    $script:StartedMock = $true
    Write-Ok "mock start" "listening on 127.0.0.1:$MockPort"

    $code = Invoke-Tool @("$script:Tools/ai_function_mock.py", "selftest", "$MockPort", "--keep-running")
    if ($code -ne 0) {
        Write-Fail "mock selftest" "the mock answered incorrectly (exit $code)"
    }
    Write-Ok "mock selftest" "all endpoints answered with the expected shape"
}

# ---------------------------------------------------------------------------
# step 4: database
# ---------------------------------------------------------------------------
Write-Step "4/7 database '$Database'"
$code = Invoke-Tool @("$script:Tools/seekdb_server.py", "exec", "--port", "$Port",
    "--user", $User, "--password=$Password",
    "--sql", "CREATE DATABASE IF NOT EXISTS $Database")
if ($code -ne 0) {
    Write-Fail "database" "could not create database '$Database' (exit $code)"
}
Write-Ok "database" "'$Database' ready"

# ---------------------------------------------------------------------------
# step 5: regression run
# ---------------------------------------------------------------------------
Write-Step "5/7 mysqltest regression against r/mysql/*.result"
$regression = @("$script:Tools/run_ai_function_tests.py",
    "--host", "127.0.0.1", "--port", "$Port",
    "--user", $User, "--password=$Password",
    "--database", $Database, "--suite", "ai_function",
    "--keep-going", "--max-diffs", "3")
if ($All) { $regression += "--all" }
if ($ExtraArgs.Count -gt 0) { $regression += $ExtraArgs }
$code = Invoke-Tool $regression
if ($code -ne 0) {
    Write-Fail "regression" "at least one case differs from its .result (exit $code); see the diff blocks above"
}
Write-Ok "regression" "ai_prompt + ai_function_enhancement$(if ($All) { ' + DDL cases' }) match the baseline"

# ---------------------------------------------------------------------------
# step 6: mock events
# ---------------------------------------------------------------------------
Write-Step "6/7 mock request summary"
& $script:Python "$script:Tools/ai_function_mock.py" "summary" "$MockPort"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[verify] WARN: could not summarise the mock events" -ForegroundColor Yellow
}
Write-Ok "mock events" "request summary printed above"

# ---------------------------------------------------------------------------
# step 7: cleanup
# ---------------------------------------------------------------------------
Write-Step "7/7 cleanup"
if ($script:StartedMock -and -not $KeepMock) {
    & $script:Python "$script:Tools/ai_function_mock.py" "stop" "$MockPort" | Out-Null
    Write-Ok "stop mock" "mock on port $MockPort stopped"
}
else {
    Write-Host "[verify] mock left running on port $MockPort" -ForegroundColor DarkGray
    Write-Ok "stop mock" "left running"
}

if ($StopSeekdb -and $script:StartedSeekdb) {
    & $script:Python "$script:Tools/seekdb_server.py" "stop" --port $Port --base-dir $BaseDir
    Write-Ok "stop seekdb" "instance on port $Port stopped (data kept in $BaseDir)"
}
elseif ($script:StartedSeekdb) {
    Write-Host "[verify] seekdb left running on port $Port (base dir $BaseDir)" -ForegroundColor DarkGray
    Write-Host "[verify]   stop it later: python tools/competition/seekdb_server.py stop --port $Port --base-dir `"$BaseDir`" [--rm]"
    Write-Ok "stop seekdb" "left running"
}
else {
    Write-Ok "stop seekdb" "not owned by this script; untouched"
}

Show-Summary
if ($script:Results | Where-Object { $_.Status -ne "ok" }) { exit 1 }
exit 0
