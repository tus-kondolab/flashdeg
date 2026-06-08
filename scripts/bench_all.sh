#!/usr/bin/env bash
# Master benchmark runner for FlashDEG / DESeq2 R / PyDESeq2 / InMoose / edgeR.
#
# Runs each benchmark tool under `/usr/bin/time -v` to capture peak RSS
# (kbytes) on the same wall clock and writes one consolidated TSV + Markdown
# summary per invocation under <results-dir>/. Pass --save-results to also
# save each tool's differential-expression result table.
#
# Usage:
#   bash scripts/bench_all.sh <counts_csv> <metadata_csv> \
#       --condition <col> --experiment <level> --control <level> \
#       [--threads <n>] [--repeats <n>] [--results-dir <dir>] \
#       [--r-env <conda_env>] [--py-env <conda_env>] \
#       [--conda-python <path>] [--conda-exe <path-or-name>] \
#       [--flashdeg-exe <path>] [--tools flashdeg,deseq2,pydeseq2,inmoose,edger] \
#       [--save-results]
#
# Example (GSE174339):
#   bash scripts/bench_all.sh counts.csv metadata.csv \
#       --condition condition --experiment BrCa --control Normal \
#       --threads 8 --repeats 1
#
# Example with explicit conda environments:
#   bash scripts/bench_all.sh counts.csv metadata.csv \
#       --condition condition --experiment BrCa --control Normal \
#       --tools flashdeg,deseq2,pydeseq2 \
#       --r-env rnaseq-r --py-env rnaseq-py \
#       --threads 8 --repeats 1 --save-results
#
# Notes:
#   - Requires GNU /usr/bin/time -v (the shell builtin `time` is not enough).
#     Default WSL/Ubuntu install provides it via the `time` apt package.
#   - --threads is passed to flashdeg / deseq2 / pydeseq2 as-is. For InMoose
#     and edgeR, the value is overridden to 1 because neither has a parallel
#     core pipeline (InMoose 0.9.1's parallel=True raises NotImplementedError;
#     edgeR's estimateDisp/glmQLFit/glmQLFTest are single-threaded and the
#     standard workflow does not pass BPPARAM). The forced value of 1 is
#     what gets recorded in the TSV/MD output for those two.
#   - --r-env selects the conda environment used for DESeq2 R / edgeR.
#     If omitted, Rscript is resolved from $PATH.
#   - --py-env selects the conda environment used for PyDESeq2 / InMoose.
#     If omitted, python is resolved from $PATH.
#   - --conda-python is kept for backward compatibility. Prefer --py-env
#     for conda environment selection; pass --conda-python <path> only if
#     you want to point at a specific Python executable.
#   - Example:
#         conda activate rnaseq-bench
#         bash scripts/bench_all.sh ...   # uses PATH Rscript/python
#   - --flashdeg-exe is optional. If omitted when running flashdeg, the
#     script auto-locates a build under the project root.
#   - --tools accepts a comma-separated subset (default: all five).
#   - --save-results writes one result CSV per tool/repeat under
#     <results-dir>/ for correctness checks. Without it, only timing
#     summaries are written.
#   - Python / R / DESeq2 / PyDESeq2 / InMoose / edgeR versions are captured into
#     the Markdown report (versions are *not* added to the TSV columns).
#   - Per-tool summary lines are still printed to console; the consolidated
#     files are timestamped so re-runs do not overwrite.

set -euo pipefail

# Pin BLAS / OMP / MKL internal threading to 1 so each tool's own
# parallelism (BiocParallel workers, DefaultInference n_cpus, FlashDEG
# --threads) is the sole source of CPU parallelism. Without this the
# BLAS layer spawns its own threads on top of each worker, causing
# oversubscription. See docs/benchmarks.md section 2.4.
export OPENBLAS_NUM_THREADS=1
export OMP_NUM_THREADS=1
export MKL_NUM_THREADS=1

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
INVOCATION_CWD="${PWD}"

usage() {
    cat <<EOF >&2
usage: bash scripts/bench_all.sh <counts_csv> <metadata_csv> \\
    --condition <col> --experiment <level> --control <level> \\
    [--threads <n>] [--repeats <n>] [--results-dir <dir>] \\
    [--r-env <conda_env>] [--py-env <conda_env>] \\
    [--conda-python <path>] [--conda-exe <path-or-name>] \\
    [--flashdeg-exe <path>] [--tools flashdeg,deseq2,pydeseq2,inmoose,edger] \\
    [--save-results]
EOF
}

POSITIONAL=()
CONDITION=""
EXPERIMENT=""
CONTROL=""
THREADS="8"
REPEATS="1"
RESULTS_DIR="results"
CONDA_PYTHON=""
R_ENV=""
PY_ENV=""
CONDA_EXE="conda"
FLASHDEG_EXE=""
TOOLS_CSV="flashdeg,deseq2,pydeseq2,inmoose,edger"
SAVE_RESULTS=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --condition)    CONDITION="$2"; shift 2 ;;
        --experiment)   EXPERIMENT="$2"; shift 2 ;;
        --control)      CONTROL="$2"; shift 2 ;;
        --threads)      THREADS="$2"; shift 2 ;;
        --repeats)      REPEATS="$2"; shift 2 ;;
        --results-dir)  RESULTS_DIR="$2"; shift 2 ;;
        --conda-python) CONDA_PYTHON="$2"; shift 2 ;;
        --r-env)        R_ENV="$2"; shift 2 ;;
        --py-env)       PY_ENV="$2"; shift 2 ;;
        --conda-exe)    CONDA_EXE="$2"; shift 2 ;;
        --flashdeg-exe) FLASHDEG_EXE="$2"; shift 2 ;;
        --tools)        TOOLS_CSV="$2"; shift 2 ;;
        --save-results) SAVE_RESULTS=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        --*)            echo "error: unknown flag: $1" >&2; usage; exit 2 ;;
        *)              POSITIONAL+=("$1"); shift ;;
    esac
done

if [[ "${#POSITIONAL[@]}" -lt 2 ]]; then usage; exit 2; fi
COUNTS="${POSITIONAL[0]}"
METADATA="${POSITIONAL[1]}"

for var_name in CONDITION EXPERIMENT CONTROL; do
    if [[ -z "${!var_name}" ]]; then
        echo "error: --${var_name,,} is required" >&2
        usage; exit 2
    fi
done

if [[ ! -f "${COUNTS}" ]]; then
    echo "error: counts CSV not found: ${COUNTS}" >&2; exit 2
fi
if [[ ! -f "${METADATA}" ]]; then
    echo "error: metadata CSV not found: ${METADATA}" >&2; exit 2
fi

# Resolve user-supplied paths to absolute (from the invocation cwd) before
# we cd into the project root for sub-bench execution.
COUNTS=$(readlink -f "${COUNTS}")
METADATA=$(readlink -f "${METADATA}")
if [[ -n "${CONDA_PYTHON}" ]] && [[ -e "${CONDA_PYTHON}" ]]; then
    CONDA_PYTHON=$(readlink -f "${CONDA_PYTHON}")
fi
if [[ -n "${PY_ENV}" && -n "${CONDA_PYTHON}" ]]; then
    echo "error: --py-env and --conda-python are mutually exclusive." >&2
    exit 2
fi
if [[ -n "${FLASHDEG_EXE}" ]]; then
    if [[ ! -f "${FLASHDEG_EXE}" ]]; then
        echo "error: --flashdeg-exe not found: ${FLASHDEG_EXE}" >&2
        exit 2
    fi
    FLASHDEG_EXE=$(readlink -f "${FLASHDEG_EXE}")
fi

if ! /usr/bin/time -v true >/dev/null 2>&1; then
    echo "error: GNU /usr/bin/time -v is required (apt install time)." >&2
    exit 1
fi

IFS=',' read -r -a TOOL_LIST <<< "${TOOLS_CSV}"

NEED_PYTHON=0
NEED_R=0
for t in "${TOOL_LIST[@]}"; do
    case "${t}" in
        pydeseq2|inmoose) NEED_PYTHON=1 ;;
        deseq2|edger)     NEED_R=1 ;;
        flashdeg)         ;;
        *) echo "error: unknown tool: ${t}" >&2; usage; exit 2 ;;
    esac
done

command_exists_or_file() {
    local exe="$1"
    if [[ -x "${exe}" ]]; then
        return 0
    fi
    command -v "${exe}" >/dev/null 2>&1
}

PY_CMD=()
PY_LABEL="PATH"
if [[ "${NEED_PYTHON}" -eq 1 ]]; then
    if [[ -n "${PY_ENV}" ]]; then
        if ! command_exists_or_file "${CONDA_EXE}"; then
            echo "error: conda executable not found: ${CONDA_EXE}" >&2
            echo "  Pass --conda-exe <path-or-name>, or omit --py-env to use python from PATH." >&2
            exit 2
        fi
        PY_CMD=("${CONDA_EXE}" run -n "${PY_ENV}" python)
        PY_LABEL="conda:${PY_ENV}"
    elif [[ -n "${CONDA_PYTHON}" ]]; then
        PY_CMD=("${CONDA_PYTHON}")
        PY_LABEL="${CONDA_PYTHON}"
    else
        if command -v python >/dev/null 2>&1; then
            PY_CMD=("$(command -v python)")
        elif command -v python3 >/dev/null 2>&1; then
            PY_CMD=("$(command -v python3)")
        else
            echo "error: no python found in PATH and no Python environment was specified." >&2
            echo "  Activate the env first (e.g. conda activate rnaseq-bench)," >&2
            echo "  or pass --py-env <conda_env>." >&2
            exit 2
        fi
        PY_LABEL="${PY_CMD[0]}"
    fi
fi

R_CMD=()
R_LABEL="PATH"
if [[ "${NEED_R}" -eq 1 ]]; then
    if [[ -n "${R_ENV}" ]]; then
        if ! command_exists_or_file "${CONDA_EXE}"; then
            echo "error: conda executable not found: ${CONDA_EXE}" >&2
            echo "  Pass --conda-exe <path-or-name>, or omit --r-env to use Rscript from PATH." >&2
            exit 2
        fi
        R_CMD=("${CONDA_EXE}" run -n "${R_ENV}" Rscript)
        R_LABEL="conda:${R_ENV}"
    else
        if ! command -v Rscript >/dev/null 2>&1; then
            echo "error: no Rscript found in PATH and --r-env not specified." >&2
            echo "  Activate the R env first (e.g. conda activate rnaseq-r)," >&2
            echo "  or pass --r-env <conda_env>." >&2
            exit 2
        fi
        R_CMD=("$(command -v Rscript)")
        R_LABEL="${R_CMD[0]}"
    fi
fi

if [[ "${NEED_PYTHON}" -eq 1 ]]; then
    if [[ -n "${CONDA_PYTHON}" && ! -x "${CONDA_PYTHON}" ]]; then
        echo "error: --conda-python not executable: ${CONDA_PYTHON}" >&2
        exit 2
    fi
    if [[ " ${TOOL_LIST[*]} " == *" pydeseq2 "* ]]; then
        if ! "${PY_CMD[@]}" -c "import pydeseq2" >/dev/null 2>&1; then
            echo "error: pydeseq2 not importable via ${PY_LABEL}" >&2
            echo "  Activate the correct env or pass --py-env <conda_env>." >&2
            exit 2
        fi
    fi
    if [[ " ${TOOL_LIST[*]} " == *" inmoose "* ]]; then
        if ! "${PY_CMD[@]}" -c "import inmoose" >/dev/null 2>&1; then
            echo "error: inmoose not importable via ${PY_LABEL}" >&2
            echo "  Activate the correct env or pass --py-env <conda_env>." >&2
            exit 2
        fi
    fi
fi

mkdir -p "${RESULTS_DIR}"
RESULTS_DIR=$(readlink -f "${RESULTS_DIR}")
TS=$(date +%Y%m%d_%H%M%S)
TSV="${RESULTS_DIR}/bench_${TS}.tsv"
MD="${RESULTS_DIR}/bench_${TS}.md"

# Run from the project root so FlashDEG auto-locate and `git rev-parse`
# resolve correctly regardless of where the user invoked bench_all.sh from.
cd "${PROJECT_ROOT}"

tool_requested() {
    local needle="$1"
    local t
    for t in "${TOOL_LIST[@]}"; do
        if [[ "${t}" == "${needle}" ]]; then
            return 0
        fi
    done
    return 1
}

locate_flashdeg_exe() {
    if [[ -n "${FLASHDEG_EXE}" ]]; then
        echo "${FLASHDEG_EXE}"
        return
    fi
    local candidate
    for candidate in \
        build-wsl/flashdeg \
        build-linux/flashdeg \
        build/flashdeg \
        build-release/flashdeg \
        build-vcpkg-ninja/flashdeg.exe \
        build-vcpkg-ninja/Release/flashdeg.exe \
        build-local/flashdeg.exe \
        flashdeg; do
        if [[ -x "${candidate}" ]]; then
            readlink -f "${candidate}"
            return
        fi
    done
    echo ""
}

if tool_requested flashdeg; then
    FLASHDEG_EXE=$(locate_flashdeg_exe)
    if [[ -z "${FLASHDEG_EXE}" ]]; then
        echo "error: FlashDEG executable not found. Pass --flashdeg-exe <path>." >&2
        exit 1
    fi
fi

WORK_DIR="${RESULTS_DIR}/_bench_${TS}_work"
mkdir -p "${WORK_DIR}"

BENCH_DESEQ2_R="${WORK_DIR}/bench_deseq2_embedded.R"
BENCH_PYDESEQ2_PY="${WORK_DIR}/bench_pydeseq2_embedded.py"
BENCH_INMOOSE_PY="${WORK_DIR}/bench_inmoose_embedded.py"
BENCH_EDGER_R="${WORK_DIR}/bench_edger_embedded.R"

write_embedded_bench_scripts() {
    cat > "${BENCH_DESEQ2_R}" <<'RSCRIPT'
suppressPackageStartupMessages(library(DESeq2))

parse_args <- function(argv) {
  cfg <- list(counts = NA_character_, metadata = NA_character_,
              condition = NA_character_, experiment = NA_character_,
              control = NA_character_, threads = 1L,
              save_results = NA_character_)
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
    cat("usage: Rscript bench_deseq2_embedded.R <counts_csv> <metadata_csv> ",
        "--condition <col> --experiment <level> --control <level> ",
        "[--threads <n>] [--save-results <path>]\n", file = stderr())
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
  cat(sprintf("error: condition column '%s' not in metadata (available: %s)\n",
              cfg$condition, paste(colnames(metadata), collapse = ", ")),
      file = stderr())
  quit(status = 2)
}

keep <- metadata[[cfg$condition]] %in% c(cfg$experiment, cfg$control)
if (sum(keep) == 0L) {
  cat(sprintf("error: no rows match condition in {%s, %s}; available values: %s\n",
              cfg$experiment, cfg$control,
              paste(sort(unique(metadata[[cfg$condition]])), collapse = ", ")),
      file = stderr())
  quit(status = 2)
}

counts <- counts[, keep, drop = FALSE]
metadata <- metadata[keep, , drop = FALSE]
metadata[[cfg$condition]] <- factor(metadata[[cfg$condition]],
                                    levels = c(cfg$control, cfg$experiment))

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

saved <- if (!is.na(cfg$save_results)) sprintf(" results=%s", cfg$save_results) else ""
cat(sprintf(
  paste0("threads=%d dds=%.3fs results=%.3fs total=%.3fs ",
         "counts=%s metadata=%s condition=%s experiment=%s control=%s%s\n"),
  cfg$threads, t_dds, t_res, t_dds + t_res,
  cfg$counts, cfg$metadata, cfg$condition, cfg$experiment, cfg$control, saved
))
RSCRIPT

    cat > "${BENCH_PYDESEQ2_PY}" <<'PYTHON'
import argparse
import sys
import time
from pathlib import Path

import pandas as pd
from pydeseq2.dds import DeseqDataSet
from pydeseq2.default_inference import DefaultInference
from pydeseq2.ds import DeseqStats


def main() -> None:
    parser = argparse.ArgumentParser(description="Timing-only PyDESeq2 benchmark.")
    parser.add_argument("counts", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--condition", required=True)
    parser.add_argument("--experiment", required=True)
    parser.add_argument("--control", required=True)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--save-results", type=Path)
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
PYTHON

    cat > "${BENCH_INMOOSE_PY}" <<'PYTHON'
import argparse
import os
import sys
import time
from pathlib import Path

import pandas as pd
from inmoose.deseq2 import DESeq, DESeqDataSet


def main() -> None:
    parser = argparse.ArgumentParser(description="Timing-only InMoose deseq2 benchmark.")
    parser.add_argument("counts", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--condition", required=True)
    parser.add_argument("--experiment", required=True)
    parser.add_argument("--control", required=True)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--save-results", type=Path)
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

    os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
    os.environ.setdefault("OMP_NUM_THREADS", "1")
    os.environ.setdefault("MKL_NUM_THREADS", "1")

    t0 = time.time()
    dds = DESeqDataSet(
        countData=counts_t,
        clinicalData=metadata,
        design=f"~{args.condition}",
    )
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
PYTHON

    cat > "${BENCH_EDGER_R}" <<'RSCRIPT'
suppressPackageStartupMessages(library(edgeR))

parse_args <- function(argv) {
  cfg <- list(counts = NA_character_, metadata = NA_character_,
              condition = NA_character_, experiment = NA_character_,
              control = NA_character_, threads = 1L,
              save_results = NA_character_)
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
    cat("usage: Rscript bench_edger_embedded.R <counts_csv> <metadata_csv> ",
        "--condition <col> --experiment <level> --control <level> ",
        "[--threads <n>] [--save-results <path>]\n", file = stderr())
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
  cat(sprintf("error: condition column '%s' not in metadata (available: %s)\n",
              cfg$condition, paste(colnames(metadata), collapse = ", ")),
      file = stderr())
  quit(status = 2)
}

keep <- metadata[[cfg$condition]] %in% c(cfg$experiment, cfg$control)
if (sum(keep) == 0L) {
  cat(sprintf("error: no rows match condition in {%s, %s}; available values: %s\n",
              cfg$experiment, cfg$control,
              paste(sort(unique(metadata[[cfg$condition]])), collapse = ", ")),
      file = stderr())
  quit(status = 2)
}

counts <- counts[, keep, drop = FALSE]
metadata <- metadata[keep, , drop = FALSE]
metadata[[cfg$condition]] <- factor(metadata[[cfg$condition]],
                                    levels = c(cfg$control, cfg$experiment))

stopifnot(all(rownames(metadata) == colnames(counts)))

count_matrix <- as.matrix(counts)
storage.mode(count_matrix) <- "integer"

t0 <- Sys.time()
y <- DGEList(counts = count_matrix, group = metadata[[cfg$condition]])
y <- calcNormFactors(y)
design <- model.matrix(~ metadata[[cfg$condition]])
t_prep <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

t0 <- Sys.time()
y <- estimateDisp(y, design)
t_disp <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

t0 <- Sys.time()
fit <- glmQLFit(y, design)
t_fit <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

t0 <- Sys.time()
qlf <- glmQLFTest(fit, coef = 2)
res <- topTags(qlf, n = Inf, sort.by = "none")$table
t_test <- as.numeric(difftime(Sys.time(), t0, units = "secs"))

total <- t_prep + t_disp + t_fit + t_test

if (!is.na(cfg$save_results)) {
  dir.create(dirname(cfg$save_results), recursive = TRUE, showWarnings = FALSE)
  res_df <- data.frame(
    gene_id = rownames(res),
    baseMean = rowMeans(count_matrix),
    log2FoldChange = res$logFC,
    lfcSE = NA_real_,
    stat = res$F,
    pvalue = res$PValue,
    padj = res$FDR,
    stringsAsFactors = FALSE
  )
  write.csv(res_df, file = cfg$save_results, row.names = FALSE, na = "NA")
}

saved <- if (!is.na(cfg$save_results)) sprintf(" results=%s", cfg$save_results) else ""
cat(sprintf(
  paste0("threads=%d prep=%.3fs disp=%.3fs fit=%.3fs test=%.3fs total=%.3fs ",
         "counts=%s metadata=%s condition=%s experiment=%s control=%s%s\n"),
  cfg$threads, t_prep, t_disp, t_fit, t_test, total,
  cfg$counts, cfg$metadata, cfg$condition, cfg$experiment, cfg$control, saved
))
RSCRIPT
}

write_embedded_bench_scripts

printf "timestamp\ttool\tthreads\trepeat\twall_s\tuser_cpu_s\tsys_cpu_s\tcpu_total_s\tcpu_pct\tpeak_rss_mb\texit_code\tresults_csv\tsummary\n" > "${TSV}"

run_one() {
    local tool="$1"
    local tool_threads="$2"
    local repeat="$3"

    local time_out stdout_out
    time_out=$(mktemp)
    stdout_out=$(mktemp)
    local result_path=""
    local flashdeg_out=""
    local profile_path=""
    if [[ "${SAVE_RESULTS}" -eq 1 ]]; then
        result_path="${RESULTS_DIR}/bench_${TS}_${tool}_r${repeat}_results.csv"
    fi

    local -a cmd
    case "${tool}" in
        flashdeg)
            profile_path="${WORK_DIR}/flashdeg_r${repeat}_profile.json"
            if [[ "${SAVE_RESULTS}" -eq 1 ]]; then
                flashdeg_out="${result_path}"
            else
                flashdeg_out="${WORK_DIR}/flashdeg_r${repeat}_results.csv"
            fi
            cmd=("${FLASHDEG_EXE}" run
                 --counts "${COUNTS}"
                 --metadata "${METADATA}"
                 --design "~ ${CONDITION}"
                 --contrast "${CONDITION}" "${EXPERIMENT}" "${CONTROL}"
                 --ref-level "${CONDITION}=${CONTROL}"
                 --refit-cooks true
                 --cooks-filter true
                 --independent-filter true
                 --threads "${tool_threads}"
                 --out "${flashdeg_out}"
                 --profile-json "${profile_path}"
                 --quiet) ;;
        deseq2)
            cmd=("${R_CMD[@]}" "${BENCH_DESEQ2_R}" "${COUNTS}" "${METADATA}"
                 --condition "${CONDITION}" --experiment "${EXPERIMENT}" --control "${CONTROL}"
                 --threads "${tool_threads}") ;;
        pydeseq2)
            cmd=("${PY_CMD[@]}" "${BENCH_PYDESEQ2_PY}" "${COUNTS}" "${METADATA}"
                 --condition "${CONDITION}" --experiment "${EXPERIMENT}" --control "${CONTROL}"
                 --threads "${tool_threads}") ;;
        inmoose)
            # InMoose 0.9.1 has no parallel implementation (parallel=True
            # raises NotImplementedError). Force tool_threads=1 so the
            # recorded value reflects the actual runtime configuration
            # rather than the misleading master --threads value.
            tool_threads=1
            cmd=("${PY_CMD[@]}" "${BENCH_INMOOSE_PY}" "${COUNTS}" "${METADATA}"
                 --condition "${CONDITION}" --experiment "${EXPERIMENT}" --control "${CONTROL}"
                 --threads "${tool_threads}") ;;
        edger)
            # edgeR's standard glmQLFit/glmQLFTest pipeline is effectively
            # single-threaded (estimateDisp / glmQLFit / glmQLFTest do not
            # take BPPARAM in the default workflow, and BLAS is pinned to
            # 1 thread for fairness). Force tool_threads=1 to mirror
            # InMoose.
            tool_threads=1
            cmd=("${R_CMD[@]}" "${BENCH_EDGER_R}" "${COUNTS}" "${METADATA}"
                 --condition "${CONDITION}" --experiment "${EXPERIMENT}" --control "${CONTROL}"
                 --threads "${tool_threads}") ;;
        *)
            echo "error: unknown tool: ${tool}" >&2
            rm -f "${time_out}" "${stdout_out}"
            return 1 ;;
    esac

    if [[ "${SAVE_RESULTS}" -eq 1 && "${tool}" != "flashdeg" ]]; then
        cmd+=(--save-results "${result_path}")
    fi

    local exit_code=0
    /usr/bin/time -v -o "${time_out}" "${cmd[@]}" >"${stdout_out}" 2>&1 || exit_code=$?

    local rss_mb=""
    if grep -q "Maximum resident set size" "${time_out}"; then
        rss_mb=$(awk '/Maximum resident set size/ {printf "%.0f", $NF/1024}' "${time_out}")
    fi

    local wall_s=""
    if grep -q "Elapsed (wall clock)" "${time_out}"; then
        wall_s=$(awk -F': ' '/Elapsed \(wall clock\)/ {
            n = split($NF, p, ":")
            s = 0
            if (n == 3)      s = p[1]*3600 + p[2]*60 + p[3]
            else if (n == 2) s = p[1]*60 + p[2]
            else             s = p[1]
            printf "%.3f", s
        }' "${time_out}")
    fi

    # CPU times. GNU time -v reports User/System with %.2f precision and
    # "Percent of CPU" with a trailing %. getrusage(RUSAGE_CHILDREN) on
    # Linux captures BiocParallel/joblib fork workers, so user_cpu_s sums
    # main process + children for DESeq2 R / PyDESeq2 / InMoose. cpu_pct
    # equals (user_cpu + sys_cpu) / wall * 100, which is the average
    # number of cores actively used.
    local user_cpu_s=""
    if grep -q "User time (seconds)" "${time_out}"; then
        user_cpu_s=$(awk -F': ' '/User time \(seconds\)/ {printf "%.3f", $NF}' "${time_out}")
    fi
    local sys_cpu_s=""
    if grep -q "System time (seconds)" "${time_out}"; then
        sys_cpu_s=$(awk -F': ' '/System time \(seconds\)/ {printf "%.3f", $NF}' "${time_out}")
    fi
    local cpu_total_s=""
    if [[ -n "${user_cpu_s}" && -n "${sys_cpu_s}" ]]; then
        cpu_total_s=$(awk -v u="${user_cpu_s}" -v s="${sys_cpu_s}" 'BEGIN {printf "%.3f", u+s}')
    fi
    local cpu_pct=""
    if grep -q "Percent of CPU" "${time_out}"; then
        cpu_pct=$(awk -F': ' '/Percent of CPU/ {gsub("%", "", $NF); printf "%s", $NF}' "${time_out}")
    fi

    local summary=""
    if [[ "${tool}" == "flashdeg" ]]; then
        local pipeline_s=""
        local wald_s=""
        if command -v python3 >/dev/null 2>&1 && [[ -f "${profile_path}" ]]; then
            pipeline_s=$(python3 - "${profile_path}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
steps = data.get("steps", {})
total_ms = 0.0
for name in (
    "size_factor_ms",
    "dispersion_gene_wise_ms",
    "dispersion_trend_ms",
    "dispersion_prior_ms",
    "dispersion_map_ms",
    "glm_fit_ms",
    "cooks_ms",
    "cook_refit_ms",
):
    total_ms += float(steps.get(name, {}).get("wall_ms", 0.0))
print(f"{total_ms / 1000.0:.3f}")
PY
)
            wald_s=$(python3 - "${profile_path}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
steps = data.get("steps", {})
print(f"{float(steps.get('wald_test_ms', {}).get('wall_ms', 0.0)) / 1000.0:.3f}")
PY
)
        fi
        if [[ -n "${pipeline_s}" && -n "${wald_s}" ]]; then
            summary=$(printf "threads=%s pipeline=%ss wald=%ss total=%ss exe=%s counts=%s metadata=%s condition=%s experiment=%s control=%s results=%s" \
                "${tool_threads}" "${pipeline_s}" "${wald_s}" "${wall_s:-?}" \
                "${FLASHDEG_EXE}" "${COUNTS}" "${METADATA}" \
                "${CONDITION}" "${EXPERIMENT}" "${CONTROL}" "${flashdeg_out}")
        else
            summary=$(printf "threads=%s total=%ss exe=%s counts=%s metadata=%s condition=%s experiment=%s control=%s results=%s" \
                "${tool_threads}" "${wall_s:-?}" "${FLASHDEG_EXE}" \
                "${COUNTS}" "${METADATA}" \
                "${CONDITION}" "${EXPERIMENT}" "${CONTROL}" "${flashdeg_out}")
        fi
    fi
    if [[ -z "${summary}" ]]; then
        summary=$(grep -E "^(threads=|n_cpus=)" "${stdout_out}" | tail -1 || true)
    fi
    if [[ -z "${summary}" ]]; then
        summary=$(tail -n 1 "${stdout_out}")
    fi

    local now
    now=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    if [[ "${SAVE_RESULTS}" -eq 1 && ( "${exit_code}" -ne 0 || ! -f "${result_path}" ) ]]; then
        result_path=""
    fi

    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "${now}" "${tool}" "${tool_threads}" "${repeat}" \
        "${wall_s}" "${user_cpu_s}" "${sys_cpu_s}" "${cpu_total_s}" "${cpu_pct}" \
        "${rss_mb}" "${exit_code}" "${result_path}" "${summary}" >> "${TSV}"

    printf "  [%s] threads=%s repeat=%s wall=%ss cpu=%ss(%s%%) peak_rss=%sMB exit=%s\n" \
        "${tool}" "${tool_threads}" "${repeat}" "${wall_s:-?}" \
        "${cpu_total_s:-?}" "${cpu_pct:-?}" "${rss_mb:-?}" "${exit_code}"

    if [[ "${exit_code}" -ne 0 ]]; then
        echo "    WARNING: ${tool} exited with ${exit_code}. Last stdout/stderr lines:" >&2
        tail -n 20 "${stdout_out}" >&2 | sed 's/^/      /' >&2
    fi

    rm -f "${time_out}" "${stdout_out}"
}

GIT_SHA=""
if git rev-parse --short HEAD >/dev/null 2>&1; then
    GIT_SHA=$(git rev-parse --short HEAD)
fi

collect_versions() {
    local versions_file="$1"
    : > "${versions_file}"
    {
        if [[ "${NEED_PYTHON}" -eq 1 ]]; then
            local py_ver
            py_ver=$("${PY_CMD[@]}" --version 2>&1 | head -1)
            echo "- Python: \`${py_ver}\` (\`${PY_LABEL}\`)"
            if [[ " ${TOOL_LIST[*]} " == *" pydeseq2 "* ]]; then
                local v
                v=$("${PY_CMD[@]}" -c "import pydeseq2; print(pydeseq2.__version__)" 2>/dev/null || echo "unknown")
                echo "- PyDESeq2: \`${v}\`"
            fi
            if [[ " ${TOOL_LIST[*]} " == *" inmoose "* ]]; then
                local v
                v=$("${PY_CMD[@]}" -c "import inmoose; print(inmoose.__version__)" 2>/dev/null || echo "unknown")
                echo "- InMoose: \`${v}\`"
            fi
        fi
        if [[ "${NEED_R}" -eq 1 ]]; then
            local r_ver
            r_ver=$("${R_CMD[@]}" -e 'cat(R.version.string)' 2>/dev/null || echo "unknown")
            echo "- R: \`${r_ver}\` (\`${R_LABEL}\`)"
            if [[ " ${TOOL_LIST[*]} " == *" deseq2 "* ]]; then
                local deseq2_ver
                deseq2_ver=$("${R_CMD[@]}" -e 'cat(as.character(packageVersion("DESeq2")))' 2>/dev/null || echo "unknown")
                echo "- DESeq2 (R): \`${deseq2_ver}\`"
                local biocparallel_ver
                biocparallel_ver=$("${R_CMD[@]}" -e 'cat(as.character(packageVersion("BiocParallel")))' 2>/dev/null || echo "unknown")
                echo "- BiocParallel (R): \`${biocparallel_ver}\`"
            fi
            if [[ " ${TOOL_LIST[*]} " == *" edger "* ]]; then
                local edger_ver
                edger_ver=$("${R_CMD[@]}" -e 'cat(as.character(packageVersion("edgeR")))' 2>/dev/null || echo "unknown")
                echo "- edgeR (R): \`${edger_ver}\`"
            fi
        fi
        if [[ " ${TOOL_LIST[*]} " == *" flashdeg "* ]]; then
            local flashdeg_version
            flashdeg_version=$("${FLASHDEG_EXE}" --version 2>/dev/null | head -1 || true)
            if [[ -n "${flashdeg_version}" ]]; then
                echo "- FlashDEG: \`${flashdeg_version}\` (\`${FLASHDEG_EXE}\`)"
                local build_info
                local git_line
                local date_line
                build_info=$("${FLASHDEG_EXE}" --build-info 2>/dev/null || true)
                git_line=$(printf '%s\n' "${build_info}" | awk '/^Git revision:/ {print; exit}')
                date_line=$(printf '%s\n' "${build_info}" | awk '/^Build date:/ {print; exit}')
                if [[ -n "${git_line}" || -n "${date_line}" ]]; then
                    if [[ -n "${git_line}" && -n "${date_line}" ]]; then
                        echo "  - ${git_line}, ${date_line}"
                    elif [[ -n "${git_line}" ]]; then
                        echo "  - ${git_line}"
                    else
                        echo "  - ${date_line}"
                    fi
                elif [[ -n "${GIT_SHA}" ]]; then
                    echo "  - Source tree git revision: ${GIT_SHA}"
                fi
            elif [[ -n "${GIT_SHA}" ]]; then
                echo "- FlashDEG: git \`${GIT_SHA}\` (source-built)"
            else
                echo "- FlashDEG: (no git SHA available)"
            fi
        fi
    } >> "${versions_file}"
}

VERSIONS_FILE=$(mktemp)
collect_versions "${VERSIONS_FILE}"
trap 'rm -f "${VERSIONS_FILE}"' EXIT

echo "Master benchmark: ${TS}"
echo "  counts:    ${COUNTS}"
echo "  metadata:  ${METADATA}"
echo "  contrast:  ${CONDITION}: ${EXPERIMENT} vs ${CONTROL}"
echo "  threads:   ${THREADS}  (forced to 1 for InMoose and edgeR; both have no parallel pipeline)"
echo "  repeats:   ${REPEATS}"
echo "  tools:     ${TOOL_LIST[*]}"
if [[ "${SAVE_RESULTS}" -eq 1 ]]; then
    echo "  results:   saved under ${RESULTS_DIR}"
else
    echo "  results:   not saved (pass --save-results to write result CSVs)"
fi
[[ "${NEED_R}" -eq 1 ]] && echo "  R env:     ${R_LABEL}"
[[ "${NEED_PYTHON}" -eq 1 ]] && echo "  Python:    ${PY_LABEL}"
echo "  blas env:  OPENBLAS_NUM_THREADS=${OPENBLAS_NUM_THREADS} OMP_NUM_THREADS=${OMP_NUM_THREADS} MKL_NUM_THREADS=${MKL_NUM_THREADS}"
echo "  host:      $(hostname)"
[[ -n "${GIT_SHA}" ]] && echo "  git:       ${GIT_SHA}"
echo "  output:    ${TSV}"
echo "             ${MD}"
echo "  versions:"
sed 's/^/    /' "${VERSIONS_FILE}"
echo ""

for tool in "${TOOL_LIST[@]}"; do
    for r in $(seq 1 "${REPEATS}"); do
        run_one "${tool}" "${THREADS}" "${r}"
    done
done

{
    echo "# Benchmark results — ${TS}"
    echo ""
    echo "- Counts: \`${COUNTS}\`"
    echo "- Metadata: \`${METADATA}\`"
    echo "- Contrast: \`${CONDITION}\` (\`${EXPERIMENT}\` vs \`${CONTROL}\`)"
    echo "- Threads: ${THREADS} (forced to 1 for InMoose and edgeR; both have no parallel pipeline)"
    if [[ "${SAVE_RESULTS}" -eq 1 ]]; then
        echo "- Result CSVs: saved under \`${RESULTS_DIR}\`"
    else
        echo "- Result CSVs: not saved (rerun with \`--save-results\` to write them)"
    fi
    echo "- BLAS env: \`OPENBLAS_NUM_THREADS=${OPENBLAS_NUM_THREADS}\` \`OMP_NUM_THREADS=${OMP_NUM_THREADS}\` \`MKL_NUM_THREADS=${MKL_NUM_THREADS}\`"
    echo "- Repeats: ${REPEATS}"
    echo "- Host: \`$(hostname)\`"
    echo "- Date: \`$(date -u +"%Y-%m-%dT%H:%M:%SZ")\`"
    echo ""
    echo "## Versions"
    echo ""
    cat "${VERSIONS_FILE}"
    echo ""
    echo "## Wall vs total vs CPU"
    echo ""
    echo "- \`Wall (s)\` below is \`/usr/bin/time -v\` Elapsed wall-clock — includes"
    echo "  interpreter startup, library import, CSV parsing, DESeq pipeline,"
    echo "  results, and exit."
    echo "- \`CPU total (s)\` is User + System CPU summed over the main process"
    echo "  and any fork children (BiocParallel workers, joblib loky workers,"
    echo "  FlashDEG thread pool). \`CPU %\` = (CPU total / Wall) × 100 ≈ the"
    echo "  average number of cores actively used. Above 100% means parallel"
    echo "  workers actively consumed extra CPU; close to 100% is single-threaded."
    echo "- The \`total=Xs\` inside each summary line is the script-internal"
    echo "  DESeq core timing only and will be smaller than Wall."
    echo ""
    echo "| Tool | Threads | Repeat | Wall (s) | CPU total (s) | CPU % | Peak RSS (MB) | Exit | Results CSV |"
    echo "|------|---------|--------|----------|---------------|-------|---------------|------|-------------|"
    awk -F'\t' 'NR>1 {result = ($12 == "" ? "not saved" : "`" $12 "`"); printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", $2, $3, $4, $5, $8, $9, $10, $11, result}' "${TSV}"
    echo ""
    echo "## Summary lines"
    echo ""
    awk -F'\t' 'NR>1 {printf "- **%s** (repeat %s): `%s`\n", $2, $4, $13}' "${TSV}"
} > "${MD}"

echo ""
echo "Done."
echo "TSV: ${TSV}"
echo "MD:  ${MD}"
