# Windows-native master benchmark runner for FlashDEG / DESeq2 R /
# PyDESeq2 / InMoose / edgeR.
#
# This mirrors scripts/bench_all.sh, but uses native Windows executables and
# conda run for tools whose environments are selected explicitly.

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $RawArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
if ([string]::IsNullOrWhiteSpace($ScriptDir)) {
    $ScriptDir = (Get-Location).Path
}
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

function Show-Usage {
    $script = "scripts\bench_all_windows.ps1"
    @"
usage:
  powershell -NoProfile -ExecutionPolicy Bypass -File $script <counts_csv> <metadata_csv> `
      --condition <col> --experiment <level> --control <level> `
      [--tools flashdeg,deseq2,pydeseq2,inmoose,edger] `
      [--threads <n>] [--repeats <n>] [--save-results] `
      [--flashdeg-exe <path>] [--r-env <conda_env>] [--py-env <conda_env>] `
      [--conda-exe <path-or-name>] [--results-dir <dir>]

example:
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\bench_all_windows.ps1 `
      data\GSE174339_counts.csv data\GSE174339_metadata.csv `
      --condition condition --experiment BrCa --control Normal `
      --tools flashdeg,deseq2,pydeseq2 `
      --threads 8 --repeats 1 --save-results `
      --r-env r460 --py-env my-python-env

notes:
  - If --tools is omitted, all benchmark tools are run:
    flashdeg,deseq2,pydeseq2,inmoose,edger.
  - If --r-env is omitted, Rscript is resolved from the current PATH.
  - If --py-env is omitted or empty, python is resolved from the current PATH.
  - InMoose and edgeR are recorded with threads=1 because these benchmark
    pipelines are effectively single-threaded.
"@
}

function Fatal([string] $Message) {
    [Console]::Error.WriteLine("error: $Message")
    [Console]::Error.WriteLine("")
    Show-Usage | ForEach-Object { [Console]::Error.WriteLine($_) }
    exit 2
}

function Read-OptionValue([int] $Index, [string] $Flag) {
    if ($Index + 1 -ge $RawArgs.Count) {
        Fatal "$Flag requires a value"
    }
    return $RawArgs[$Index + 1]
}

$Condition = $null
$Experiment = $null
$Control = $null
$Threads = 1
$Repeats = 1
$ToolsArg = "flashdeg,deseq2,pydeseq2,inmoose,edger"
$ResultsDir = Join-Path $ScriptDir "results"
$SaveResults = $false
$FlashdegExeArg = $null
$REnv = $null
$PyEnv = $null
$CondaExe = "conda"
$PollMs = 250
$Positionals = New-Object System.Collections.Generic.List[string]

for ($i = 0; $i -lt $RawArgs.Count; ) {
    $arg = $RawArgs[$i]
    switch ($arg) {
        "--help" {
            Show-Usage
            exit 0
        }
        "-h" {
            Show-Usage
            exit 0
        }
        "--condition" {
            $Condition = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--experiment" {
            $Experiment = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--control" {
            $Control = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--threads" {
            $Threads = [int](Read-OptionValue $i $arg)
            $i += 2
            continue
        }
        "--repeats" {
            $Repeats = [int](Read-OptionValue $i $arg)
            $i += 2
            continue
        }
        "--tools" {
            $ToolsArg = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--results-dir" {
            $ResultsDir = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--save-results" {
            $SaveResults = $true
            $i += 1
            continue
        }
        "--flashdeg-exe" {
            $FlashdegExeArg = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--r-env" {
            $REnv = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--py-env" {
            $PyEnv = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--conda-exe" {
            $CondaExe = Read-OptionValue $i $arg
            $i += 2
            continue
        }
        "--poll-ms" {
            $PollMs = [int](Read-OptionValue $i $arg)
            $i += 2
            continue
        }
        default {
            if ($arg.StartsWith("--")) {
                Fatal "unknown flag: $arg"
            }
            $Positionals.Add($arg)
            $i += 1
        }
    }
}

if ($Positionals.Count -lt 2) {
    Fatal "counts_csv and metadata_csv are required"
}
if ([string]::IsNullOrWhiteSpace($Condition)) { Fatal "--condition is required" }
if ([string]::IsNullOrWhiteSpace($Experiment)) { Fatal "--experiment is required" }
if ([string]::IsNullOrWhiteSpace($Control)) { Fatal "--control is required" }
if ($Threads -lt 1) { Fatal "--threads must be >= 1" }
if ($Repeats -lt 1) { Fatal "--repeats must be >= 1" }
if ($PollMs -lt 50) { Fatal "--poll-ms must be >= 50" }

function Resolve-InputFile([string] $Path, [string] $Label) {
    $candidate = $Path
    if (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path (Get-Location).Path $candidate
    }
    if (-not [System.IO.File]::Exists($candidate)) {
        Fatal "$Label not found: $Path"
    }
    return (Resolve-Path $candidate).Path
}

function Resolve-OutputDir([string] $Path) {
    $candidate = $Path
    if (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path (Get-Location).Path $candidate
    }
    New-Item -ItemType Directory -Force -Path $candidate | Out-Null
    return (Resolve-Path $candidate).Path
}

$Counts = Resolve-InputFile $Positionals[0] "counts CSV"
$Metadata = Resolve-InputFile $Positionals[1] "metadata CSV"
$ResultsDir = Resolve-OutputDir $ResultsDir

$ValidTools = @("flashdeg", "deseq2", "pydeseq2", "inmoose", "edger")
$ToolList = @(
    $ToolsArg -split "," |
        ForEach-Object { $_.Trim().ToLowerInvariant() } |
        Where-Object { $_ -ne "" }
)
if ($ToolList.Count -eq 0) {
    Fatal "--tools produced an empty tool list"
}
foreach ($tool in $ToolList) {
    if ($ValidTools -notcontains $tool) {
        Fatal "unknown tool '$tool' (valid: $($ValidTools -join ', '))"
    }
}

function Resolve-OptionalFile([string] $Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }
    $candidate = $Path
    if (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path (Get-Location).Path $candidate
    }
    if (-not [System.IO.File]::Exists($candidate)) {
        Fatal "file not found: $Path"
    }
    return (Resolve-Path $candidate).Path
}

function Find-FlashdegExe {
    if (-not [string]::IsNullOrWhiteSpace($FlashdegExeArg)) {
        return Resolve-OptionalFile $FlashdegExeArg
    }
    $candidates = @(
        (Join-Path $RepoRoot "build-vcpkg-ninja\flashdeg.exe"),
        (Join-Path $RepoRoot "build-vcpkg-ninja\Release\flashdeg.exe"),
        (Join-Path $RepoRoot "build\release\flashdeg.exe"),
        (Join-Path $RepoRoot "build\flashdeg.exe"),
        (Join-Path $RepoRoot "flashdeg.exe")
    )
    foreach ($candidate in $candidates) {
        if ([System.IO.File]::Exists($candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }
    Fatal "FlashDEG executable not found; pass --flashdeg-exe <path>"
}

$FlashdegExe = $null
if ($ToolList -contains "flashdeg") {
    $FlashdegExe = Find-FlashdegExe
}

function Quote-CommandArg([string] $Arg) {
    if ($null -eq $Arg -or $Arg.Length -eq 0) {
        return '""'
    }
    if ($Arg -notmatch '[\s"]') {
        return $Arg
    }
    return '"' + ($Arg -replace '"', '\"') + '"'
}

function Join-CommandArgs([string[]] $CommandArgs) {
    return (($CommandArgs | ForEach-Object { Quote-CommandArg $_ }) -join " ")
}

function Format-Command([string] $File, [string[]] $CommandArgs) {
    $items = @((Quote-CommandArg $File)) + @($CommandArgs | ForEach-Object { Quote-CommandArg $_ })
    return ($items -join " ")
}

function Get-ProcessTreeIds([int] $RootPid) {
    $ids = New-Object System.Collections.Generic.List[int]
    $queue = New-Object System.Collections.Queue
    $seen = @{}
    $ids.Add($RootPid)
    $queue.Enqueue($RootPid)
    $seen[$RootPid] = $true

    try {
        $all = Get-CimInstance Win32_Process -ErrorAction Stop
    } catch {
        return $ids.ToArray()
    }

    $childrenByParent = @{}
    foreach ($p in $all) {
        $parent = [int]$p.ParentProcessId
        if (-not $childrenByParent.ContainsKey($parent)) {
            $childrenByParent[$parent] = New-Object System.Collections.Generic.List[int]
        }
        $childrenByParent[$parent].Add([int]$p.ProcessId)
    }

    while ($queue.Count -gt 0) {
        $parent = [int]$queue.Dequeue()
        if (-not $childrenByParent.ContainsKey($parent)) {
            continue
        }
        foreach ($child in $childrenByParent[$parent]) {
            if ($seen.ContainsKey($child)) {
                continue
            }
            $seen[$child] = $true
            $ids.Add([int]$child)
            $queue.Enqueue([int]$child)
        }
    }
    return $ids.ToArray()
}

function Get-TreeWorkingSetMb([int] $RootPid) {
    $sum = [int64]0
    foreach ($id in Get-ProcessTreeIds $RootPid) {
        try {
            $p = Get-Process -Id $id -ErrorAction Stop
            $sum += [int64]$p.WorkingSet64
        } catch {
            # The process may exit between the tree scan and Get-Process.
        }
    }
    return [math]::Round($sum / 1MB)
}

function Invoke-ProcessMeasured(
    [string] $File,
    [string[]] $ArgumentList,
    [string] $StdoutPath,
    [string] $StderrPath,
    [int] $PollIntervalMs
) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = Join-CommandArgs $ArgumentList
    $psi.WorkingDirectory = $RepoRoot
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$proc.Start()
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()

    $peakMb = 0
    Start-Sleep -Milliseconds 10
    try {
        $proc.Refresh()
        if (-not $proc.HasExited) {
            $peakMb = [math]::Round($proc.WorkingSet64 / 1MB)
        }
    } catch {
        $peakMb = 0
    }
    while (-not $proc.HasExited) {
        try {
            $proc.Refresh()
            $rootMb = [math]::Round($proc.WorkingSet64 / 1MB)
            if ($rootMb -gt $peakMb) {
                $peakMb = $rootMb
            }
        } catch {
            # Keep the previous sample.
        }
        $currentMb = Get-TreeWorkingSetMb $proc.Id
        if ($currentMb -gt $peakMb) {
            $peakMb = $currentMb
        }
        Start-Sleep -Milliseconds $PollIntervalMs
    }
    $proc.WaitForExit()
    $finalMb = Get-TreeWorkingSetMb $proc.Id
    if ($finalMb -gt $peakMb) {
        $peakMb = $finalMb
    }
    try {
        $proc.Refresh()
        $rootPeakMb = [math]::Round($proc.PeakWorkingSet64 / 1MB)
        if ($rootPeakMb -gt $peakMb) {
            $peakMb = $rootPeakMb
        }
    } catch {
        # PeakWorkingSet64 can be unavailable for very short-lived wrappers.
    }
    $sw.Stop()

    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    [System.IO.File]::WriteAllText($StdoutPath, $stdout, [System.Text.Encoding]::UTF8)
    [System.IO.File]::WriteAllText($StderrPath, $stderr, [System.Text.Encoding]::UTF8)

    return [PSCustomObject]@{
        ExitCode = $proc.ExitCode
        WallSeconds = $sw.Elapsed.TotalSeconds
        PeakWorkingSetMb = $peakMb
        Stdout = $stdout
        Stderr = $stderr
        Command = Format-Command $File $ArgumentList
    }
}

function Invoke-CaptureLine([string] $File, [string[]] $ArgumentList) {
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $File
        $psi.Arguments = Join-CommandArgs $ArgumentList
        $psi.WorkingDirectory = $RepoRoot
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $p = New-Object System.Diagnostics.Process
        $p.StartInfo = $psi
        [void]$p.Start()
        $stdout = $p.StandardOutput.ReadToEnd()
        $stderr = $p.StandardError.ReadToEnd()
        $p.WaitForExit()
        if ($p.ExitCode -ne 0) {
            $line = (($stderr -split "`r?`n") | Where-Object { $_.Trim() -ne "" } | Select-Object -First 1)
            if ([string]::IsNullOrWhiteSpace($line)) { return "unknown" }
            return "unknown ($line)"
        }
        $first = (($stdout -split "`r?`n") | Where-Object { $_.Trim() -ne "" } | Select-Object -First 1)
        if ([string]::IsNullOrWhiteSpace($first)) { return "unknown" }
        return $first.Trim()
    } catch {
        return "unknown ($($_.Exception.Message))"
    }
}

function Invoke-CaptureLines([string] $File, [string[]] $ArgumentList) {
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $File
        $psi.Arguments = Join-CommandArgs $ArgumentList
        $psi.WorkingDirectory = $RepoRoot
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $p = New-Object System.Diagnostics.Process
        $p.StartInfo = $psi
        [void]$p.Start()
        $stdout = $p.StandardOutput.ReadToEnd()
        [void]$p.StandardError.ReadToEnd()
        $p.WaitForExit()
        if ($p.ExitCode -ne 0) {
            return @()
        }
        return @(
            $stdout -split "`r?`n" |
                Where-Object { $_.Trim() -ne "" } |
                ForEach-Object { $_.Trim() }
        )
    } catch {
        return @()
    }
}

function New-RCommand([string[]] $InnerArgs) {
    if ([string]::IsNullOrWhiteSpace($REnv)) {
        return [PSCustomObject]@{ File = "Rscript"; Args = $InnerArgs }
    }
    return [PSCustomObject]@{
        File = $CondaExe
        Args = @("run", "-n", $REnv, "Rscript") + $InnerArgs
    }
}

function New-PythonCommand([string[]] $InnerArgs) {
    if ([string]::IsNullOrWhiteSpace($PyEnv)) {
        return [PSCustomObject]@{ File = "python"; Args = $InnerArgs }
    }
    return [PSCustomObject]@{
        File = $CondaExe
        Args = @("run", "-n", $PyEnv, "python") + $InnerArgs
    }
}

function Get-EnvLabel([string] $EnvName, [string] $DirectName) {
    if ([string]::IsNullOrWhiteSpace($EnvName)) {
        return $DirectName
    }
    return $EnvName
}

function Get-LastSummaryLine([string] $Path) {
    if (-not [System.IO.File]::Exists($Path)) {
        return ""
    }
    $lines = [System.IO.File]::ReadAllLines($Path)
    for ($i = $lines.Length - 1; $i -ge 0; --$i) {
        $line = $lines[$i].Trim()
        if ($line.StartsWith("threads=") -or $line.StartsWith("n_cpus=")) {
            return $line
        }
    }
    for ($i = $lines.Length - 1; $i -ge 0; --$i) {
        $line = $lines[$i].Trim()
        if ($line -ne "") {
            return $line
        }
    }
    return ""
}

function Get-LastLines([string] $Path, [int] $Count) {
    if (-not [System.IO.File]::Exists($Path)) {
        return @()
    }
    $lines = [System.IO.File]::ReadAllLines($Path)
    if ($lines.Length -le $Count) {
        return $lines
    }
    return $lines[($lines.Length - $Count)..($lines.Length - 1)]
}

function Sanitize-Tsv([object] $Value) {
    if ($null -eq $Value) {
        return ""
    }
    return ([string]$Value) -replace "`t", " " -replace "`r", " " -replace "`n", " "
}

$env:OPENBLAS_NUM_THREADS = "1"
$env:OMP_NUM_THREADS = "1"
$env:MKL_NUM_THREADS = "1"

$RunId = "windows_" + (Get-Date -Format "yyyyMMdd_HHmmss")
$TsvPath = Join-Path $ResultsDir ("bench_{0}.tsv" -f $RunId)
$MdPath = Join-Path $ResultsDir ("bench_{0}.md" -f $RunId)
$WorkDir = Join-Path $ResultsDir ("_bench_{0}_work" -f $RunId)
$LogDir = Join-Path $ResultsDir ("bench_{0}_logs" -f $RunId)
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$NeedR = @($ToolList | Where-Object { $_ -eq "deseq2" -or $_ -eq "edger" }).Count -gt 0
$NeedPython = @($ToolList | Where-Object { $_ -eq "pydeseq2" -or $_ -eq "inmoose" }).Count -gt 0

$VersionLines = New-Object System.Collections.Generic.List[string]
if ($ToolList -contains "flashdeg") {
    $VersionLines.Add("- FlashDEG: " + (Invoke-CaptureLine $FlashdegExe @("--version")) + " (" + $FlashdegExe + ")")
    $buildInfo = Invoke-CaptureLines $FlashdegExe @("--build-info")
    $gitLine = ($buildInfo | Where-Object { $_ -match "^Git revision:" } | Select-Object -First 1)
    $dateLine = ($buildInfo | Where-Object { $_ -match "^Build date:" } | Select-Object -First 1)
    $buildParts = @()
    if ($null -ne $gitLine) { $buildParts += $gitLine }
    if ($null -ne $dateLine) { $buildParts += $dateLine }
    if ($buildParts.Count -gt 0) {
        $VersionLines.Add("  - " + ($buildParts -join ", "))
    }
}
if ($NeedR) {
    $cmd = New-RCommand @("-e", "cat(R.version.string)")
    $VersionLines.Add("- R: " + (Invoke-CaptureLine $cmd.File $cmd.Args) + " (env: " + (Get-EnvLabel $REnv "PATH") + ")")
    if ($ToolList -contains "deseq2") {
        $cmd = New-RCommand @("-e", "cat(as.character(packageVersion('DESeq2')))")
        $VersionLines.Add("- DESeq2 (R): " + (Invoke-CaptureLine $cmd.File $cmd.Args))
        $cmd = New-RCommand @("-e", "cat(as.character(packageVersion('BiocParallel')))")
        $VersionLines.Add("- BiocParallel (R): " + (Invoke-CaptureLine $cmd.File $cmd.Args))
    }
    if ($ToolList -contains "edger") {
        $cmd = New-RCommand @("-e", "cat(as.character(packageVersion('edgeR')))")
        $VersionLines.Add("- edgeR (R): " + (Invoke-CaptureLine $cmd.File $cmd.Args))
    }
}
if ($NeedPython) {
    $cmd = New-PythonCommand @("--version")
    $VersionLines.Add("- Python: " + (Invoke-CaptureLine $cmd.File $cmd.Args) + " (env: " + (Get-EnvLabel $PyEnv "PATH") + ")")
    if ($ToolList -contains "pydeseq2") {
        $cmd = New-PythonCommand @("-c", "import pydeseq2; print(pydeseq2.__version__)")
        $VersionLines.Add("- PyDESeq2: " + (Invoke-CaptureLine $cmd.File $cmd.Args))
    }
    if ($ToolList -contains "inmoose") {
        $cmd = New-PythonCommand @("-c", "import inmoose; print(inmoose.__version__)")
        $VersionLines.Add("- InMoose: " + (Invoke-CaptureLine $cmd.File $cmd.Args))
    }
}

[System.IO.File]::WriteAllText(
    $TsvPath,
    "timestamp`ttool`tthreads`trepeat`twall_s`tuser_cpu_s`tsys_cpu_s`tcpu_total_s`tcpu_pct`tpeak_rss_mb`texit_code`tresults_csv`tsummary`n",
    [System.Text.Encoding]::UTF8
)

Write-Host "Windows benchmark: $RunId"
Write-Host "  counts:    $Counts"
Write-Host "  metadata:  $Metadata"
Write-Host "  contrast:  ${Condition}: $Experiment vs $Control"
Write-Host "  threads:   $Threads  (forced to 1 for InMoose and edgeR)"
Write-Host "  repeats:   $Repeats"
Write-Host "  tools:     $($ToolList -join ',')"
Write-Host "  R env:     $(Get-EnvLabel $REnv 'PATH')"
Write-Host "  Python env:$(Get-EnvLabel $PyEnv 'PATH')"
if ($ToolList -contains "flashdeg") { Write-Host "  flashdeg:  $FlashdegExe" }
Write-Host "  blas env:  OPENBLAS_NUM_THREADS=$env:OPENBLAS_NUM_THREADS OMP_NUM_THREADS=$env:OMP_NUM_THREADS MKL_NUM_THREADS=$env:MKL_NUM_THREADS"
Write-Host "  output:    $TsvPath"
Write-Host "             $MdPath"
Write-Host "  versions:"
foreach ($line in $VersionLines) {
    Write-Host "    $line"
}
Write-Host ""

$Rows = New-Object System.Collections.Generic.List[object]

function Invoke-ToolRun([string] $Tool, [int] $Repeat) {
    $effectiveThreads = $Threads
    if ($Tool -eq "inmoose" -or $Tool -eq "edger") {
        $effectiveThreads = 1
    }

    $resultPath = ""
    $internalResultPath = Join-Path $WorkDir ("{0}_r{1}_results.csv" -f $Tool, $Repeat)
    if ($SaveResults) {
        $resultPath = Join-Path $ResultsDir ("bench_{0}_{1}_r{2}_results.csv" -f $RunId, $Tool, $Repeat)
    }

    $stdoutPath = Join-Path $LogDir ("{0}_r{1}.stdout.txt" -f $Tool, $Repeat)
    $stderrPath = Join-Path $LogDir ("{0}_r{1}.stderr.txt" -f $Tool, $Repeat)

    switch ($Tool) {
        "flashdeg" {
            $outPath = if ($SaveResults) { $resultPath } else { $internalResultPath }
            $profilePath = Join-Path $WorkDir ("{0}_r{1}_profile.json" -f $Tool, $Repeat)
            $file = $FlashdegExe
            $procArgs = @(
                "run",
                "--counts", $Counts,
                "--metadata", $Metadata,
                "--design", ("~ " + $Condition),
                "--contrast", $Condition, $Experiment, $Control,
                "--ref-level", ($Condition + "=" + $Control),
                "--refit-cooks", "true",
                "--cooks-filter", "true",
                "--independent-filter", "true",
                "--threads", [string]$effectiveThreads,
                "--out", $outPath,
                "--profile-json", $profilePath,
                "--quiet"
            )
        }
        "deseq2" {
            $scriptArgs = @(
                (Join-Path $ScriptDir "bench_deseq2.R"),
                $Counts,
                $Metadata,
                "--condition", $Condition,
                "--experiment", $Experiment,
                "--control", $Control,
                "--threads", [string]$effectiveThreads
            )
            if ($SaveResults) {
                $scriptArgs += @("--save-results", $resultPath)
            }
            $cmd = New-RCommand $scriptArgs
            $file = $cmd.File
            $procArgs = $cmd.Args
        }
        "pydeseq2" {
            $scriptArgs = @(
                (Join-Path $ScriptDir "bench_pydeseq2.py"),
                $Counts,
                $Metadata,
                "--condition", $Condition,
                "--experiment", $Experiment,
                "--control", $Control,
                "--threads", [string]$effectiveThreads
            )
            if ($SaveResults) {
                $scriptArgs += @("--save-results", $resultPath)
            }
            $cmd = New-PythonCommand $scriptArgs
            $file = $cmd.File
            $procArgs = $cmd.Args
        }
        "inmoose" {
            $scriptArgs = @(
                (Join-Path $ScriptDir "bench_inmoose.py"),
                $Counts,
                $Metadata,
                "--condition", $Condition,
                "--experiment", $Experiment,
                "--control", $Control,
                "--threads", [string]$effectiveThreads
            )
            if ($SaveResults) {
                $scriptArgs += @("--save-results", $resultPath)
            }
            $cmd = New-PythonCommand $scriptArgs
            $file = $cmd.File
            $procArgs = $cmd.Args
        }
        "edger" {
            $scriptArgs = @(
                (Join-Path $ScriptDir "bench_edger.R"),
                $Counts,
                $Metadata,
                "--condition", $Condition,
                "--experiment", $Experiment,
                "--control", $Control,
                "--threads", [string]$effectiveThreads
            )
            if ($SaveResults) {
                $scriptArgs += @("--save-results", $resultPath)
            }
            $cmd = New-RCommand $scriptArgs
            $file = $cmd.File
            $procArgs = $cmd.Args
        }
        default {
            Fatal "unknown tool: $Tool"
        }
    }

    $measurement = Invoke-ProcessMeasured $file $procArgs $stdoutPath $stderrPath $PollMs
    $summary = Get-LastSummaryLine $stdoutPath
    if ($Tool -eq "flashdeg" -and [string]::IsNullOrWhiteSpace($summary)) {
        $saved = if ($SaveResults) { $resultPath } else { $internalResultPath }
        $summary = "threads=$effectiveThreads total=$([math]::Round($measurement.WallSeconds, 3))s exe=$FlashdegExe counts=$Counts metadata=$Metadata condition=$Condition experiment=$Experiment control=$Control results=$saved"
    }

    $resultForTsv = ""
    if ($SaveResults -and $measurement.ExitCode -eq 0 -and [System.IO.File]::Exists($resultPath)) {
        $resultForTsv = $resultPath
    }

    $now = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    $line = @(
        (Sanitize-Tsv $now),
        (Sanitize-Tsv $Tool),
        (Sanitize-Tsv $effectiveThreads),
        (Sanitize-Tsv $Repeat),
        (Sanitize-Tsv ("{0:F3}" -f $measurement.WallSeconds)),
        "",
        "",
        "",
        "",
        (Sanitize-Tsv $measurement.PeakWorkingSetMb),
        (Sanitize-Tsv $measurement.ExitCode),
        (Sanitize-Tsv $resultForTsv),
        (Sanitize-Tsv $summary)
    ) -join "`t"
    [System.IO.File]::AppendAllText($TsvPath, $line + "`n", [System.Text.Encoding]::UTF8)

    $row = [PSCustomObject]@{
        Tool = $Tool
        Threads = $effectiveThreads
        Repeat = $Repeat
        WallSeconds = $measurement.WallSeconds
        PeakWorkingSetMb = $measurement.PeakWorkingSetMb
        ExitCode = $measurement.ExitCode
        ResultsCsv = $resultForTsv
        Summary = $summary
        StdoutLog = $stdoutPath
        StderrLog = $stderrPath
        Command = $measurement.Command
    }
    $Rows.Add($row)

    Write-Host ("  [{0}] threads={1} repeat={2} wall={3:F3}s peak_ws={4}MB exit={5}" -f `
        $Tool, $effectiveThreads, $Repeat, $measurement.WallSeconds, $measurement.PeakWorkingSetMb, $measurement.ExitCode)

    if ($measurement.ExitCode -ne 0) {
        Write-Host "    WARNING: $Tool exited with $($measurement.ExitCode). Last stdout/stderr lines:"
        foreach ($line in Get-LastLines $stdoutPath 10) { Write-Host "      $line" }
        foreach ($line in Get-LastLines $stderrPath 10) { Write-Host "      $line" }
        Write-Host "    stdout log: $stdoutPath"
        Write-Host "    stderr log: $stderrPath"
    }
}

foreach ($tool in $ToolList) {
    for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
        Invoke-ToolRun $tool $repeat
    }
}

$MdLines = New-Object System.Collections.Generic.List[string]
$MdLines.Add("# Windows benchmark results - $RunId")
$MdLines.Add("")
$MdLines.Add("- Counts: ``$Counts``")
$MdLines.Add("- Metadata: ``$Metadata``")
$MdLines.Add("- Contrast: ``$Condition`` (``$Experiment`` vs ``$Control``)")
$MdLines.Add("- Threads: $Threads (forced to 1 for InMoose and edgeR)")
$MdLines.Add("- Repeats: $Repeats")
$MdLines.Add("- Result CSVs: " + $(if ($SaveResults) { "saved under ``$ResultsDir``" } else { "not saved; pass ``--save-results`` to write them" }))
$MdLines.Add("- Logs: ``$LogDir``")
$MdLines.Add("- BLAS env: ``OPENBLAS_NUM_THREADS=$env:OPENBLAS_NUM_THREADS`` ``OMP_NUM_THREADS=$env:OMP_NUM_THREADS`` ``MKL_NUM_THREADS=$env:MKL_NUM_THREADS``")
$MdLines.Add("- Date: ``$((Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))``")
$MdLines.Add("")
$MdLines.Add("## Versions")
$MdLines.Add("")
foreach ($line in $VersionLines) { $MdLines.Add($line) }
$MdLines.Add("")
$MdLines.Add("## Timing")
$MdLines.Add("")
$MdLines.Add("- Wall time is measured by PowerShell Stopwatch around the launched native command.")
$MdLines.Add("- Peak RSS is reported as peak Windows working set MB for the launched process tree, sampled every $PollMs ms.")
$MdLines.Add("- CPU columns in the TSV are intentionally blank because Windows has no GNU /usr/bin/time -v equivalent here.")
$MdLines.Add("")
$MdLines.Add("| Tool | Threads | Repeat | Wall (s) | Peak working set (MB) | Exit | Results CSV |")
$MdLines.Add("|------|---------|--------|----------|------------------------|------|-------------|")
foreach ($row in $Rows) {
    $resultCell = if ([string]::IsNullOrWhiteSpace($row.ResultsCsv)) { "not saved" } else { "``$($row.ResultsCsv)``" }
    $MdLines.Add(("| {0} | {1} | {2} | {3:F3} | {4} | {5} | {6} |" -f `
        $row.Tool, $row.Threads, $row.Repeat, $row.WallSeconds, $row.PeakWorkingSetMb, $row.ExitCode, $resultCell))
}
$MdLines.Add("")
$MdLines.Add("## Summary lines")
$MdLines.Add("")
foreach ($row in $Rows) {
    $MdLines.Add(("- **{0}** (repeat {1}): ``{2}``" -f $row.Tool, $row.Repeat, $row.Summary))
}
$MdLines.Add("")
$MdLines.Add("## Commands")
$MdLines.Add("")
foreach ($row in $Rows) {
    $MdLines.Add(("- **{0}** (repeat {1}): ``{2}``" -f $row.Tool, $row.Repeat, $row.Command))
}

[System.IO.File]::WriteAllLines($MdPath, $MdLines, [System.Text.Encoding]::UTF8)

Write-Host ""
Write-Host "Done."
Write-Host "TSV: $TsvPath"
Write-Host "MD:  $MdPath"
