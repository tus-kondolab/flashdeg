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
  - If --flashdeg-exe is omitted, this script searches repo/release build
    outputs, C:\Program Files\FlashDEG, then the current PATH.
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
        (Join-Path $RepoRoot "flashdeg.exe"),
        (Join-Path $env:ProgramFiles "FlashDEG\flashdeg.exe")
    )
    if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} "FlashDEG\flashdeg.exe")
    }
    foreach ($candidate in $candidates) {
        if ([System.IO.File]::Exists($candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }
    foreach ($commandName in @("flashdeg.exe", "flashdeg")) {
        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace($command.Source) -and [System.IO.File]::Exists($command.Source)) {
            return (Resolve-Path $command.Source).Path
        }
    }
    Fatal "FlashDEG executable not found in repo build outputs, C:\Program Files\FlashDEG, or PATH; pass --flashdeg-exe <path>"
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

function Get-EmbeddedBenchmarkScripts {
    return [PSCustomObject]@{
        Deseq2 = @'
# Timing-only DESeq2 R benchmark.
#
# Usage:
#   Rscript tools/bench_deseq2.R <counts_csv> <metadata_csv> \
#       --condition <col> --experiment <level> --control <level> \
#       [--threads <n>] [--save-results <path>]
#
# Example (GSE174339):
#   Rscript tools/bench_deseq2.R counts.csv metadata.csv \
#       --condition condition --experiment BrCa --control Normal --threads 8
#
# Arguments:
#   counts_csv    Path to counts CSV (gene_id as first column, samples
#                 as remaining columns).
#   metadata_csv  Path to metadata CSV (sample_id as first column).
#   --condition   Metadata column holding the contrast factor.
#   --experiment  Experimental / test level (e.g. 'BrCa').
#   --control     Reference / control level (e.g. 'Normal').
#                 Only rows whose condition is either experiment or
#                 control are kept; any other levels are dropped.
#   --threads     BiocParallel MulticoreParam(workers = N) worker count
#                 (default: 1). Empirically optimal: any N > 1 with
#                 DESeq(parallel = TRUE) and results(parallel = FALSE);
#                 see docs/benchmarks.md section 6.2.
#   --save-results Optional CSV path for the DESeq2 results table.
#
# Memory: wrap with /usr/bin/time -v to capture peak RSS.
#
# Reproducibility note: this script depends on DESeq2 and (for
# threads > 1) BiocParallel. Recommended environment: Ubuntu /
# WSL2 R 4.4.x with Bioconductor 3.20.

suppressPackageStartupMessages(library(DESeq2))

parse_args <- function(argv) {
  cfg <- list(
    counts = NA_character_,
    metadata = NA_character_,
    condition = NA_character_,
    experiment = NA_character_,
    control = NA_character_,
    threads = 1L,
    save_results = NA_character_
  )
  positional <- character(0)
  i <- 1L
  while (i <= length(argv)) {
    tok <- argv[i]
    if (tok == "--condition") {
      cfg$condition <- argv[i + 1L]; i <- i + 2L
    } else if (tok == "--experiment") {
      cfg$experiment <- argv[i + 1L]; i <- i + 2L
    } else if (tok == "--control") {
      cfg$control <- argv[i + 1L]; i <- i + 2L
    } else if (tok == "--threads") {
      cfg$threads <- as.integer(argv[i + 1L]); i <- i + 2L
    } else if (tok == "--save-results") {
      cfg$save_results <- argv[i + 1L]; i <- i + 2L
    } else if (startsWith(tok, "--")) {
      stop(sprintf("unknown flag: %s", tok), call. = FALSE)
    } else {
      positional <- c(positional, tok); i <- i + 1L
    }
  }
  if (length(positional) < 2L) {
    cat(
      "usage: Rscript tools/bench_deseq2.R <counts_csv> <metadata_csv> ",
      "--condition <col> --experiment <level> --control <level> ",
      "[--threads <n>]\n",
      file = stderr()
    )
    quit(status = 2)
  }
  cfg$counts <- positional[1L]
  cfg$metadata <- positional[2L]
  for (k in c("condition", "experiment", "control")) {
    if (is.na(cfg[[k]])) {
      cat(sprintf("error: --%s is required\n", k), file = stderr())
      quit(status = 2)
    }
  }
  if (is.na(cfg$threads)) cfg$threads <- 1L
  cfg
}

cfg <- parse_args(commandArgs(trailingOnly = TRUE))

if (!file.exists(cfg$counts)) {
  cat(sprintf("error: counts CSV not found: %s\n", cfg$counts), file = stderr())
  quit(status = 2)
}
if (!file.exists(cfg$metadata)) {
  cat(sprintf("error: metadata CSV not found: %s\n", cfg$metadata), file = stderr())
  quit(status = 2)
}

counts <- read.csv(cfg$counts, row.names = 1, check.names = FALSE)
metadata <- read.csv(cfg$metadata, row.names = 1, check.names = FALSE)

if (!(cfg$condition %in% colnames(metadata))) {
  cat(sprintf(
    "error: condition column '%s' not in metadata (available: %s)\n",
    cfg$condition, paste(colnames(metadata), collapse = ", ")
  ), file = stderr())
  quit(status = 2)
}

keep <- metadata[[cfg$condition]] %in% c(cfg$experiment, cfg$control)
if (sum(keep) == 0L) {
  cat(sprintf(
    "error: no rows match condition in {%s, %s}; available values: %s\n",
    cfg$experiment, cfg$control,
    paste(sort(unique(metadata[[cfg$condition]])), collapse = ", ")
  ), file = stderr())
  quit(status = 2)
}

counts <- counts[, keep, drop = FALSE]
metadata <- metadata[keep, , drop = FALSE]
metadata[[cfg$condition]] <- factor(
  metadata[[cfg$condition]],
  levels = c(cfg$control, cfg$experiment)
)

stopifnot(all(rownames(metadata) == colnames(counts)))

count_matrix <- as.matrix(counts)
storage.mode(count_matrix) <- "integer"

if (cfg$threads > 1L) {
  suppressPackageStartupMessages(library(BiocParallel))
  register(MulticoreParam(workers = cfg$threads))
}

design_formula <- as.formula(paste0("~", cfg$condition))

t0 <- Sys.time()
dds <- DESeqDataSetFromMatrix(countData = count_matrix,
                              colData = metadata,
                              design = design_formula)
# Apply the empirically optimal DESeq2 threading policy (see
# docs/benchmarks.md section 6.2):
#   - DESeq(parallel=TRUE) when workers > 1: gives modest speedup
#   - results(parallel=FALSE) always: BiocParallel overhead exceeds the
#     work in independent filtering / BH adjustment / Cook filtering
#     and consistently slows results() down by ~20% on this workload.
dds <- DESeq(dds, quiet = TRUE, parallel = (cfg$threads > 1L))
t_dds <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

t0 <- Sys.time()
res <- results(dds, contrast = c(cfg$condition, cfg$experiment, cfg$control),
               parallel = FALSE)
if (!is.na(cfg$save_results)) {
  dir.create(dirname(cfg$save_results), recursive = TRUE, showWarnings = FALSE)
  res_df <- as.data.frame(res)
  res_df <- data.frame(gene_id = rownames(res_df), res_df, check.names = FALSE)
  write.csv(res_df, file = cfg$save_results, row.names = FALSE, na = "NA")
}
t_res <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

saved <- if (!is.na(cfg$save_results)) {
  sprintf(" results=%s", cfg$save_results)
} else {
  ""
}
cat(sprintf(
  paste0(
    "threads=%d dds=%.3fs results=%.3fs total=%.3fs ",
    "counts=%s metadata=%s condition=%s experiment=%s control=%s%s\n"
  ),
  cfg$threads, t_dds, t_res, t_dds + t_res,
  cfg$counts, cfg$metadata, cfg$condition, cfg$experiment, cfg$control,
  saved
))
'@
        Pydeseq2 = @'
"""Timing-only PyDESeq2 benchmark.

Usage:
    python3 tools/bench_pydeseq2.py <counts_csv> <metadata_csv> \
        --condition <col> --experiment <level> --control <level> \
        [--threads <n>] [--save-results <path>]

Example (GSE174339):
    python3 tools/bench_pydeseq2.py counts.csv metadata.csv \
        --condition condition --experiment BrCa --control Normal --threads 8

Arguments:
    counts_csv    Path to the counts CSV (gene_id as first column, samples
                  as remaining columns).
    metadata_csv  Path to the metadata CSV (sample_id as first column).
    --condition   Metadata column holding the contrast factor.
    --experiment  The experimental / test level value of that column.
    --control     The control / reference level value of that column.
                  Only rows whose condition is either ``experiment`` or
                  ``control`` are kept; any other levels are dropped.
    --threads     PyDESeq2 ``DefaultInference(n_cpus=N)`` worker count
                  (default: 1).

Prints a one-line summary of dds and wald timings. When
``--save-results`` is supplied, writes the PyDESeq2 Wald result table.

Memory: wrap with ``/usr/bin/time -v`` to capture peak RSS.

Reproducibility note: this script must be run with a Python interpreter
that has PyDESeq2 installed. When using scripts/bench_all.sh, select that
environment with ``--py-env <conda_env>`` or activate it before running.
"""
import argparse
import sys
import time
from pathlib import Path

import pandas as pd
from pydeseq2.dds import DeseqDataSet
from pydeseq2.default_inference import DefaultInference
from pydeseq2.ds import DeseqStats


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Timing-only PyDESeq2 benchmark."
    )
    parser.add_argument("counts", type=Path, help="counts CSV path")
    parser.add_argument("metadata", type=Path, help="metadata CSV path")
    parser.add_argument(
        "--condition", required=True,
        help="metadata column name (e.g. 'condition')"
    )
    parser.add_argument(
        "--experiment", required=True,
        help="experimental/test level (e.g. 'BrCa')"
    )
    parser.add_argument(
        "--control", required=True,
        help="reference/control level (e.g. 'Normal')"
    )
    parser.add_argument(
        "--threads", type=int, default=1,
        help="PyDESeq2 DefaultInference n_cpus (default: 1)"
    )
    parser.add_argument(
        "--save-results", type=Path,
        help="optional CSV path for the PyDESeq2 Wald result table"
    )
    args = parser.parse_args()

    if not args.counts.is_file():
        sys.exit(f"error: counts CSV not found: {args.counts}")
    if not args.metadata.is_file():
        sys.exit(f"error: metadata CSV not found: {args.metadata}")

    counts = pd.read_csv(args.counts, index_col=0)
    metadata = pd.read_csv(args.metadata, index_col=0)

    if args.condition not in metadata.columns:
        sys.exit(
            f"error: condition column '{args.condition}' not in metadata "
            f"(available: {list(metadata.columns)})"
        )

    keep_mask = metadata[args.condition].isin([args.experiment, args.control])
    if keep_mask.sum() == 0:
        sys.exit(
            f"error: no rows match condition in "
            f"{{{args.experiment}, {args.control}}}; "
            f"available values: {sorted(metadata[args.condition].unique())}"
        )

    counts = counts[metadata.index[keep_mask]]
    metadata = metadata.loc[keep_mask].copy()
    metadata[args.condition] = pd.Categorical(
        metadata[args.condition], categories=[args.control, args.experiment]
    )
    counts_t = counts.T.astype(int)

    t0 = time.time()
    dds = DeseqDataSet(
        counts=counts_t,
        metadata=metadata,
        design=f"~ {args.condition}",
        refit_cooks=True,
        inference=DefaultInference(n_cpus=args.threads),
        quiet=True,
    )
    dds.deseq2()
    t_dds = time.time() - t0

    t0 = time.time()
    stats = DeseqStats(
        dds,
        contrast=[args.condition, args.experiment, args.control],
        quiet=True,
    )
    stats.summary()
    if args.save_results is not None:
        args.save_results.parent.mkdir(parents=True, exist_ok=True)
        if getattr(stats, "results_df", None) is None:
            sys.exit("error: PyDESeq2 did not populate stats.results_df")
        stats.results_df.to_csv(args.save_results, index_label="gene_id", na_rep="NA")
    t_wald = time.time() - t0

    saved = f" results={args.save_results}" if args.save_results is not None else ""
    print(
        f"n_cpus={args.threads} dds={t_dds:.3f}s wald={t_wald:.3f}s "
        f"total={t_dds + t_wald:.3f}s "
        f"counts={args.counts} metadata={args.metadata} "
        f"condition={args.condition} experiment={args.experiment} "
        f"control={args.control}{saved}"
    )


if __name__ == "__main__":
    main()
'@
        Inmoose = @'
"""Timing-only InMoose deseq2 benchmark.

Usage:
    python3 tools/bench_inmoose.py <counts_csv> <metadata_csv> \
        --condition <col> --experiment <level> --control <level> \
        [--threads <n>] [--save-results <path>]

Example (GSE174339):
    python3 tools/bench_inmoose.py counts.csv metadata.csv \
        --condition condition --experiment BrCa --control Normal --threads 1

Arguments:
    counts_csv    Path to the counts CSV.
    metadata_csv  Path to the metadata CSV.
    --condition   Metadata column holding the contrast factor.
    --experiment  Experimental/test level (e.g., 'BrCa').
    --control     Reference/control level (e.g., 'Normal').
                  Only rows whose condition is either ``experiment`` or
                  ``control`` are kept; any other levels are dropped.
    --threads     Reported only as metadata. InMoose 0.9.1 raises
                  ``NotImplementedError`` for ``parallel=True``; this argument
                  has no effect on InMoose's internal parallelism (default: 1).

Prints a one-line summary of dds and results timings. When
``--save-results`` is supplied, writes the InMoose result table.

Memory: wrap with ``/usr/bin/time -v`` to capture peak RSS.

Reproducibility note: this script must be run with a Python interpreter
that has InMoose installed. When using scripts/bench_all.sh, select that
environment with ``--py-env <conda_env>`` or activate it before running.
"""
import argparse
import os
import sys
import time
from pathlib import Path

import pandas as pd
from inmoose.deseq2 import DESeq, DESeqDataSet


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Timing-only InMoose deseq2 benchmark."
    )
    parser.add_argument("counts", type=Path, help="counts CSV path")
    parser.add_argument("metadata", type=Path, help="metadata CSV path")
    parser.add_argument(
        "--condition", required=True,
        help="metadata column name (e.g. 'condition')"
    )
    parser.add_argument(
        "--experiment", required=True,
        help="experimental/test level (e.g. 'BrCa')"
    )
    parser.add_argument(
        "--control", required=True,
        help="reference/control level (e.g. 'Normal')"
    )
    parser.add_argument(
        "--threads", type=int, default=1,
        help="reported for parity; InMoose 0.9.1 is single-threaded only"
    )
    parser.add_argument(
        "--save-results", type=Path,
        help="optional CSV path for the InMoose result table"
    )
    args = parser.parse_args()

    if not args.counts.is_file():
        sys.exit(f"error: counts CSV not found: {args.counts}")
    if not args.metadata.is_file():
        sys.exit(f"error: metadata CSV not found: {args.metadata}")

    counts = pd.read_csv(args.counts, index_col=0)
    metadata = pd.read_csv(args.metadata, index_col=0)

    if args.condition not in metadata.columns:
        sys.exit(
            f"error: condition column '{args.condition}' not in metadata "
            f"(available: {list(metadata.columns)})"
        )

    keep_mask = metadata[args.condition].isin([args.experiment, args.control])
    if keep_mask.sum() == 0:
        sys.exit(
            f"error: no rows match condition in "
            f"{{{args.experiment}, {args.control}}}; "
            f"available values: {sorted(metadata[args.condition].unique())}"
        )

    counts = counts[metadata.index[keep_mask]]
    metadata = metadata.loc[keep_mask].copy()
    metadata[args.condition] = pd.Categorical(
        metadata[args.condition], categories=[args.control, args.experiment]
    )

    # InMoose expects countData as samples-by-genes (DESeq2 R-like).
    counts_t = counts.T.astype(int)

    # InMoose threading via env vars (joblib / OpenBLAS).
    os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
    os.environ.setdefault("OMP_NUM_THREADS", "1")
    os.environ.setdefault("MKL_NUM_THREADS", "1")

    t0 = time.time()
    dds = DESeqDataSet(
        countData=counts_t,
        clinicalData=metadata,
        design=f"~{args.condition}",
    )
    # InMoose 0.9.1 does not implement own parallelism.
    dds = DESeq(dds, quiet=True, parallel=False)
    t_dds = time.time() - t0

    t0 = time.time()
    res = dds.results(
        contrast=[args.condition, args.experiment, args.control],
        parallel=False,
    )
    if args.save_results is not None:
        args.save_results.parent.mkdir(parents=True, exist_ok=True)
        if hasattr(res, "to_csv"):
            res.to_csv(args.save_results, index_label="gene_id", na_rep="NA")
        elif hasattr(res, "results_df") and hasattr(res.results_df, "to_csv"):
            res.results_df.to_csv(args.save_results, index_label="gene_id", na_rep="NA")
        else:
            sys.exit(f"error: unsupported InMoose result type: {type(res)!r}")
    t_res = time.time() - t0

    saved = f" results={args.save_results}" if args.save_results is not None else ""
    print(
        f"threads={args.threads}(noop, single-threaded only) "
        f"dds={t_dds:.3f}s results={t_res:.3f}s "
        f"total={t_dds + t_res:.3f}s "
        f"counts={args.counts} metadata={args.metadata} "
        f"condition={args.condition} experiment={args.experiment} "
        f"control={args.control}{saved}"
    )


if __name__ == "__main__":
    main()
'@
        Edger = @'
# Timing-only edgeR benchmark.
#
# Usage:
#   Rscript tools/bench_edger.R <counts_csv> <metadata_csv> \
#       --condition <col> --experiment <level> --control <level> \
#       [--threads <n>] [--save-results <path>]
#
# Example (GSE174339):
#   Rscript tools/bench_edger.R counts.csv metadata.csv \
#       --condition condition --experiment BrCa --control Normal --threads 8
#
# Arguments:
#   counts_csv    Path to counts CSV (gene_id as first column, samples
#                 as remaining columns).
#   metadata_csv  Path to metadata CSV (sample_id as first column).
#   --condition   Metadata column holding the contrast factor.
#   --experiment  Experimental / test level (e.g. 'BrCa').
#   --control     Reference / control level (e.g. 'Normal'). Only rows
#                 whose condition is one of these two levels are kept.
#   --threads     Recorded into the summary line but has no runtime effect.
#                 edgeR's standard glmQLFit/glmQLFTest pipeline is
#                 effectively single-threaded -- estimateDisp, glmQLFit,
#                 and glmQLFTest run their C/Fortran inner loops on a
#                 single thread, and the typical workflow does not pass
#                 BPPARAM= to any function. BLAS-bound matrix multiplies
#                 can be sped up by setting OPENBLAS_NUM_THREADS > 1
#                 externally, but bench_all.sh pins those to 1 for cross-
#                 tool fairness, so in benchmark runs edgeR consumes ~one
#                 CPU core regardless of --threads. The flag exists so
#                 the script signature matches bench_deseq2.R etc.
#                 Default: 1.
#   --save-results Optional CSV path for the edgeR result table written in
#                 DESeq2-compatible schema:
#                 gene_id, baseMean, log2FoldChange, lfcSE, stat, pvalue, padj.
#
# Memory: wrap with /usr/bin/time -v to capture peak RSS.
#
# Mirrors the one-line summary format of tools/bench_deseq2.R,
# tools/bench_pydeseq2.py, tools/bench_inmoose.py, and
# tools/bench_flashdeg.sh for cross-tool comparison.
#
# Reproducibility note: requires the edgeR Bioconductor package. Tested
# with edgeR 4.x on R 4.5.x. The pipeline is the standard edgeR glmQLFit /
# glmQLFTest workflow without filterByExpr filtering, so all input genes
# are retained (consistent with the other bench scripts).

suppressPackageStartupMessages(library(edgeR))

parse_args <- function(argv) {
  cfg <- list(
    counts = NA_character_,
    metadata = NA_character_,
    condition = NA_character_,
    experiment = NA_character_,
    control = NA_character_,
    threads = 1L,
    save_results = NA_character_
  )
  positional <- character(0)
  i <- 1L
  while (i <= length(argv)) {
    tok <- argv[i]
    if (tok == "--condition") {
      cfg$condition <- argv[i + 1L]; i <- i + 2L
    } else if (tok == "--experiment") {
      cfg$experiment <- argv[i + 1L]; i <- i + 2L
    } else if (tok == "--control") {
      cfg$control <- argv[i + 1L]; i <- i + 2L
    } else if (tok == "--threads") {
      cfg$threads <- as.integer(argv[i + 1L]); i <- i + 2L
    } else if (tok == "--save-results") {
      cfg$save_results <- argv[i + 1L]; i <- i + 2L
    } else if (startsWith(tok, "--")) {
      stop(sprintf("unknown flag: %s", tok), call. = FALSE)
    } else {
      positional <- c(positional, tok); i <- i + 1L
    }
  }
  if (length(positional) < 2L) {
    cat(
      "usage: Rscript tools/bench_edger.R <counts_csv> <metadata_csv> ",
      "--condition <col> --experiment <level> --control <level> ",
      "[--threads <n>] [--save-results <path>]\n",
      file = stderr()
    )
    quit(status = 2)
  }
  cfg$counts <- positional[1L]
  cfg$metadata <- positional[2L]
  for (k in c("condition", "experiment", "control")) {
    if (is.na(cfg[[k]])) {
      cat(sprintf("error: --%s is required\n", k), file = stderr())
      quit(status = 2)
    }
  }
  if (is.na(cfg$threads)) cfg$threads <- 1L
  cfg
}

cfg <- parse_args(commandArgs(trailingOnly = TRUE))

if (!file.exists(cfg$counts)) {
  cat(sprintf("error: counts CSV not found: %s\n", cfg$counts), file = stderr())
  quit(status = 2)
}
if (!file.exists(cfg$metadata)) {
  cat(sprintf("error: metadata CSV not found: %s\n", cfg$metadata), file = stderr())
  quit(status = 2)
}

counts <- read.csv(cfg$counts, row.names = 1, check.names = FALSE)
metadata <- read.csv(cfg$metadata, row.names = 1, check.names = FALSE)

if (!(cfg$condition %in% colnames(metadata))) {
  cat(sprintf(
    "error: condition column '%s' not in metadata (available: %s)\n",
    cfg$condition, paste(colnames(metadata), collapse = ", ")
  ), file = stderr())
  quit(status = 2)
}

keep <- metadata[[cfg$condition]] %in% c(cfg$experiment, cfg$control)
if (sum(keep) == 0L) {
  cat(sprintf(
    "error: no rows match condition in {%s, %s}; available values: %s\n",
    cfg$experiment, cfg$control,
    paste(sort(unique(metadata[[cfg$condition]])), collapse = ", ")
  ), file = stderr())
  quit(status = 2)
}

counts <- counts[, keep, drop = FALSE]
metadata <- metadata[keep, , drop = FALSE]
metadata[[cfg$condition]] <- factor(
  metadata[[cfg$condition]],
  levels = c(cfg$control, cfg$experiment)
)

stopifnot(all(rownames(metadata) == colnames(counts)))

count_matrix <- as.matrix(counts)
storage.mode(count_matrix) <- "integer"

# Stage 1: DGEList construction + TMM normalization + design matrix.
t0 <- Sys.time()
y <- DGEList(counts = count_matrix, group = metadata[[cfg$condition]])
y <- calcNormFactors(y)
design <- model.matrix(~ metadata[[cfg$condition]])
t_prep <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

# Stage 2: Empirical Bayes dispersion estimation.
t0 <- Sys.time()
y <- estimateDisp(y, design)
t_disp <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

# Stage 3: GLM quasi-likelihood fit.
t0 <- Sys.time()
fit <- glmQLFit(y, design)
t_fit <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

# Stage 4: QL F-test on the contrast coefficient (experiment vs control).
t0 <- Sys.time()
qlf <- glmQLFTest(fit, coef = 2)
res <- topTags(qlf, n = Inf, sort.by = "none")$table
t_test <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

total <- t_prep + t_disp + t_fit + t_test

if (!is.na(cfg$save_results)) {
  dir.create(dirname(cfg$save_results), recursive = TRUE, showWarnings = FALSE)
  # edgeR's QL F-test does not expose a per-coefficient standard error in
  # topTags output. We populate the DESeq2-compatible schema with NA for
  # lfcSE and write the F statistic into 'stat'. Consumers comparing edgeR
  # to DESeq2 / FlashDEG should treat 'stat' as F (not Wald z) and skip
  # lfcSE-based comparisons.
  res_df <- data.frame(
    gene_id        = rownames(res),
    baseMean       = rowMeans(count_matrix),
    log2FoldChange = res$logFC,
    lfcSE          = NA_real_,
    stat           = res$F,
    pvalue         = res$PValue,
    padj           = res$FDR,
    stringsAsFactors = FALSE
  )
  write.csv(res_df, file = cfg$save_results, row.names = FALSE, na = "NA")
}

saved <- if (!is.na(cfg$save_results)) {
  sprintf(" results=%s", cfg$save_results)
} else {
  ""
}
cat(sprintf(
  paste0(
    "threads=%d prep=%.3fs disp=%.3fs fit=%.3fs test=%.3fs total=%.3fs ",
    "counts=%s metadata=%s condition=%s experiment=%s control=%s%s\n"
  ),
  cfg$threads, t_prep, t_disp, t_fit, t_test, total,
  cfg$counts, cfg$metadata, cfg$condition, cfg$experiment, cfg$control,
  saved
))
'@
    }
}

function ConvertTo-RInlineExpression([string] $Code) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Code)
    $hexParts = New-Object System.Collections.Generic.List[string]
    foreach ($byte in $bytes) {
        $hexParts.Add($byte.ToString("x2"))
    }
    $hex = [string]::Join("", $hexParts)
    return "h <- '$hex'; i <- seq(1, nchar(h), 2); code <- rawToChar(as.raw(strtoi(substring(h, i, i + 1), 16L))); eval(parse(text = code))"
}

function ConvertTo-PythonInlineExpression([string] $Code) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Code)
    $encoded = [System.Convert]::ToBase64String($bytes)
    return "import base64; exec(base64.b64decode('$encoded').decode('utf-8'))"
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

$EmbeddedBenchScripts = Get-EmbeddedBenchmarkScripts
$BenchDeseq2Expr = ConvertTo-RInlineExpression $EmbeddedBenchScripts.Deseq2
$BenchPydeseq2Expr = ConvertTo-PythonInlineExpression $EmbeddedBenchScripts.Pydeseq2
$BenchInmooseExpr = ConvertTo-PythonInlineExpression $EmbeddedBenchScripts.Inmoose
$BenchEdgerExpr = ConvertTo-RInlineExpression $EmbeddedBenchScripts.Edger

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
                "-e",
                $BenchDeseq2Expr,
                "--args",
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
                "-c",
                $BenchPydeseq2Expr,
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
                "-c",
                $BenchInmooseExpr,
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
                "-e",
                $BenchEdgerExpr,
                "--args",
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
