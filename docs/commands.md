# FlashDEG Command Reference

This document summarizes the user-facing FlashDEG command-line interface.
It focuses on normal analysis commands and omits developer-only oracle/debug
details except where they affect release behavior.

## Commands

| Command | Purpose |
|---|---|
| `flashdeg --help`, `flashdeg -h` | Show top-level help. |
| `flashdeg --version` | Show the FlashDEG version. |
| `flashdeg --build-info` | Show git revision, build date, numerical backends, and build flags. |
| `flashdeg run --help`, `flashdeg run -h` | Show options for the analysis command. |
| `flashdeg run [options]` | Run differential expression analysis from a count matrix and metadata table. |

The only analysis subcommand currently implemented is `run`.

## Basic Usage

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ condition" \
  --ref-level "condition=control" \
  --contrast "condition" "treated" "control" \
  --out results.csv
```

This compares `treated` against `control` and reports
`log2(treated/control)`.

Use `--ref-level` and `--contrast` together in standard analyses.
`--contrast` controls the reported comparison direction, while `--ref-level`
controls the reference level used in the design matrix.

## Input Files

| Option | Description |
|---|---|
| `--counts <path>` | Count matrix. Required. FlashDEG expects genes/features as rows and samples as columns by default. `.tsv` and `.tab` extensions are read as tab-delimited files. |
| `--metadata <path>` | Sample metadata table. Required. `.tsv` and `.tab` extensions are read as tab-delimited files. |
| `--features-as-cols` | Read counts as samples x genes instead of the default genes x samples layout. |
| `--tximport-round` | Allow non-integer estimated counts and round them with R-compatible half-to-even rounding before analysis. Intended for tximport counts generated with `countsFromAbundance="scaledTPM"` or `"lengthScaledTPM"`; length-offset normalization is not applied. |

The first column of the counts file is treated as the gene ID column.
The first column of the metadata file is treated as the sample ID column.
Metadata sample IDs must match the sample names in the count matrix.

## Model And Contrast Options

| Option | Description |
|---|---|
| `--design <formula>` | Model formula, for example `"~ condition"`, `"~ batch + condition"`, or `"~ genotype + treatment + genotype:treatment"`. |
| `--ref-level <column=level>` | Reference level for a factor. Example: `"condition=control"`. Repeat this option for multiple factors. |
| `--contrast <column> <test> <reference>` | Compare two groups from a metadata column. Example: `--contrast "condition" "treated" "control"`. |
| `--contrast-name <design-column>` | Test one design-matrix column by exact name. Useful for interaction terms. |

Exactly one of `--contrast`, `--contrast-name`, or `--contrast-vector` must be
specified.

### Advanced Design-Matrix Input

Use `--design-matrix` and `--contrast-vector` only when you have already
prepared the design matrix yourself, for example in R or Python. This is useful
for custom coding, transformed continuous covariates, or exact model-matrix
reproduction across tools.

| Option | Description |
|---|---|
| `--design-matrix <path>` | Precomputed design matrix CSV. Mutually exclusive with `--design`. |
| `--contrast-vector <comma-separated numbers>` | Column-weight specification for `--design-matrix`. Provide one number for each design-matrix column. For example, if columns are `Intercept, condition[T.treated]`, then `0,1` tests `condition[T.treated]`; if columns are `Intercept, condition[T.B], condition[T.C]`, then `0,-1,1` tests `C` vs `B`. Required with `--design-matrix`. |

## Common Examples

### Two-Group Comparison With Batch Adjustment

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ batch + condition" \
  --ref-level "condition=control" \
  --contrast "condition" "treated" "control" \
  --out results.csv
```

### Interaction-Term Test

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ genotype + treatment + genotype:treatment" \
  --ref-level "genotype=WT" \
  --ref-level "treatment=ctrl" \
  --contrast-name "genotype[T.KO]:treatment[T.drug]" \
  --out interaction_results.csv
```

### Precomputed Design Matrix

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design-matrix design_matrix.csv \
  --contrast-vector "0,1" \
  --out results.csv
```

## Likelihood-Ratio Test (LRT)

By default `flashdeg run` performs a Wald test. Pass `--test LRT` to compare the
full `--design` against a nested `--reduced` model instead. The LRT is useful for
testing whether *any* level of a multi-level factor contributes, or whether an
interaction as a whole is supported.

| Option | Description |
|---|---|
| `--test Wald\|LRT` | Statistical test for the result table. Default: `Wald`. |
| `--reduced <formula>` | Nested reduced-model formula (use with a `--design` formula). Example: `"~ 1"` or `"~ batch"`. |
| `--reduced-design-matrix <path>` | Nested reduced design matrix (use with `--design-matrix`). |

The reduced model must be nested within the full model and have fewer effective
parameters; FlashDEG validates this and uses the rank difference as the test
degrees of freedom. An intercept-only reduced model is written `"~ 1"`.

**The contrast is for display only under LRT.** One of `--contrast`,
`--contrast-name`, or `--contrast-vector` is still required, but it only selects
the `log2FoldChange` and `lfcSE` reported in the table. The `stat` and `pvalue`
test the full design against the reduced design (all dropped terms jointly), so
they are not described by the single displayed contrast.

Output columns are unchanged
(`gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj`). Under LRT, `stat` is
the likelihood-ratio statistic `2*(logLik_full - logLik_reduced)` and `pvalue` is
its chi-square survival probability at the test degrees of freedom.

`--test LRT` cannot be combined with `--lfc-shrink`, `--lfc-null`, or
`--alt-hypothesis`, which are Wald-only.

### LRT Examples

Test the whole `condition` factor against an intercept-only model:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ condition" \
  --test LRT \
  --reduced "~ 1" \
  --contrast "condition" "treated" "control" \
  --out lrt_results.csv
```

Test `condition` while keeping `batch` in both models:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ batch + condition" \
  --test LRT \
  --reduced "~ batch" \
  --contrast "condition" "treated" "control" \
  --out lrt_results.csv
```

Test an interaction term as a whole:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ genotype + treatment + genotype:treatment" \
  --test LRT \
  --reduced "~ genotype + treatment" \
  --contrast-name "genotype[T.KO]:treatment[T.drug]" \
  --out interaction_lrt.csv
```

With precomputed design matrices, pass both the full and reduced matrices:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design-matrix full_design.csv \
  --reduced-design-matrix reduced_design.csv \
  --test LRT \
  --contrast-name "condition[T.treated]" \
  --out lrt_results.csv
```

## Analysis Mode And Numerical Options

| Option | Description |
|---|---|
| `--fit-type parametric\|local\|mean` | Dispersion trend fit. The default is `parametric`. |
| `--size-factors ratio\|poscounts` | Size-factor estimator. Most users should leave this unset. `ratio` is the default median-ratio method. Use `poscounts` for sparse data where many genes contain zeros. |
| `--min-mu <value>` | Lower bound for fitted means used by dispersion and Wald fitting. |
| `--min-disp <value>` | Lower bound for dispersion estimates. |
| `--max-disp <value>` | Upper bound for dispersion estimates. |
| `--beta-tol <value>` | Convergence tolerance for beta fitting. |
| `--threads <n>` | Worker thread count. `0` means automatic selection. |
| `--deterministic` | Force single-thread deterministic execution. |
| `--quiet` | Suppress nonessential informational output. |
| `--dry-run` | Validate inputs and options without writing analysis results. |

## Wald Test, Filtering, And Cook Outliers

| Option | Description |
|---|---|
| `--out <path>` | Write the main Wald result CSV. |
| `--alpha <value>` | Target FDR used by independent filtering. If omitted, the default is `0.1`. |
| `--independent-filter true\|false` | Enable or disable independent filtering. |
| `--refit-cooks true\|false` | Enable or disable Cook outlier replacement and refit. Default: `true`. |
| `--cooks-filter true\|false` | Enable or disable Cook outlier p-value filtering. Default: `true`. |
| `--lfc-null <value>` | Null log2 fold-change for Wald testing. The ordinary null is `0`. |
| `--alt-hypothesis greaterAbs\|lessAbs\|greater\|less` | Alternative hypothesis used with nonzero `--lfc-null`. |
| `--min-replicates <n>` | Minimum replicate count used by Cook replacement logic. |

## Main Output

The CSV written by `--out` contains one row per gene and the following columns:

```text
gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj
```

Additional user-facing output files can be requested with these options:

| Option | Description |
|---|---|
| `--write-size-factors <path>` | Write sample size factors. |
| `--write-normalized-counts <path>` | Write normalized counts. |
| `--write-base-means <path>` | Write per-gene base means. |
| `--write-fitted-dispersions <path>` | Write fitted trend dispersions. |
| `--write-dispersions <path>` | Write final dispersions. |
| `--write-replaced-counts <path>` | Write the count matrix after Cook outlier replacement. |
| `--write-pvalue-cooks-outlier <path>` | Write the mask of genes whose p-values were filtered by Cook outlier logic. |
| `--write-replaced <path>` | Write `1.0` for genes with replaced Cook outliers. |
| `--write-refitted <path>` | Write `1.0` for replaced genes that were refitted. |
| `--write-new-all-zeroes <path>` | Write `1.0` for refitted genes that became all-zero after replacement. |
| `--write-independent-filtering <path>` | Write independent-filtering candidate cutoffs, rejection counts, and selected cutoff. |

## VST

Use variance-stabilizing transformation output like this:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ condition" \
  --ref-level "condition=control" \
  --contrast "condition" "treated" "control" \
  --vst \
  --write-vst-counts vst_counts.csv \
  --out results.csv
```

| Option | Description |
|---|---|
| `--vst` | Compute variance-stabilized counts. |
| `--vst-blind` | Fit VST with an intercept-only design. This is the default VST behavior. |
| `--vst-use-design` | Fit VST with the analysis design. |
| `--vst-fit-type parametric\|mean` | VST trend fit type. |
| `--write-vst-counts <path>` | Write the VST count matrix. |

## LFC Shrinkage

Use apeGLM-style shrunken LFC output like this:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ condition" \
  --ref-level "condition=control" \
  --contrast "condition" "treated" "control" \
  --lfc-shrink \
  --lfc-shrink-coef "condition[T.treated]" \
  --write-shrunken-lfc shrunken_lfc.csv \
  --out results.csv
```

| Option | Description |
|---|---|
| `--lfc-shrink` | Compute shrunken LFC values. |
| `--lfc-shrink-coef <name>` | Design-matrix column to shrink. |
| `--lfc-shrink-no-adapt` | Disable empirical prior adaptation and use prior scale 1. |
| `--write-shrunken-lfc <path>` | Write `baseMean`, `shrunkenLog2FoldChange`, `shrunkenLfcSE`, `pvalue`, `padj`, and `converged`. |

Use the ordinary Wald result `pvalue` and `padj` columns for DEG calling.
Use shrunken LFC values for effect-size visualization and ranking.

## Profiling

| Option | Description |
|---|---|
| `--profile` | Print profile timings. |
| `--profile-cpu` | Include CPU profile information when supported by the current build. |
| `--profile-json <path>` | Write profile information as JSON. |

## Developer-Only Options

Release builds hide or reject developer-only compatibility and oracle/debug
options.
The complete developer-only option list is:

```text
--compat-mode pydeseq2|deseq2-r
--write-design-matrix
--write-rough-dispersions
--write-moments-dispersions
--write-mom-dispersions
--write-genewise-dispersions
--write-dispersion-iterations
--write-dispersion-outliers
--write-mu-hat
--write-map-dispersions
--write-lfc
--write-lfc-log2
--write-lfc-converged
--write-lfc-iterations
--write-lfc-fallback
--write-mu-lfc
--write-hat-diagonals
--write-cooks
--write-replace-cooks
--write-vst-trend-coeffs
```

These options are intended for developer builds configured with
`FLASHDEG_ENABLE_DEV_OPTIONS=ON`.

## Reserved But Not Implemented

| Option | Status |
|---|---|
| `--log <path>` | Reserved for future logging support. Currently rejected as unsupported. |
| `--silence-warnings` | Reserved for future warning control. Currently rejected as unsupported. |
