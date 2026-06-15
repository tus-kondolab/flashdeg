# DESeq2 Compatibility

This page summarizes which DESeq2 R features FlashDEG supports, which supported
features have known compatibility caveats, and which features are unsupported.

## At a Glance

FlashDEG supports the core workflow needed for many bulk RNA-seq DEG analyses:

- gene-by-sample count matrix input,
- sample metadata input,
- size-factor normalization,
- dispersion estimation,
- negative-binomial GLM fitting,
- Wald-test differential expression,
- likelihood-ratio tests against a nested reduced model,
- `baseMean`, `log2FoldChange`, `lfcSE`, `stat`, `pvalue`, and `padj` output,
- BH adjusted p-values with independent filtering,
- Cook outlier filtering and optional replacement/refit,
- one-factor, additive multi-factor, and two-way interaction designs.

Features outside that core fall into two groups:

- **Supported with caveats**: available in FlashDEG, with a documented
  compatibility boundary.
- **Unsupported**: not available in FlashDEG.

## Core DEG Features

All features in this table are supported.

| DESeq2 feature | Compatibility note |
|---|---|
| `DESeq(..., test="Wald")` | Standard DEG calling path. |
| `DESeq(..., test="LRT", reduced=...)` | Likelihood-ratio test of the full design against a nested reduced model (`--test LRT --reduced`). |
| `results()` Wald table | Outputs the standard result columns: `baseMean`, `log2FoldChange`, `lfcSE`, `stat`, `pvalue`, `padj`. |
| Size factors: `ratio` | Default size-factor method. |
| Size factors: `poscounts` | Useful when many genes contain zeros. |
| Dispersion `fitType="parametric"` | Standard dispersion trend for DESeq2-compatible DEG calling. |
| Dispersion `fitType="mean"` | Available when a mean trend is selected. |
| Negative-binomial GLM fitting | Used for Wald statistics. |
| Independent filtering | Uses BH-adjusted p-values. |
| Cook filtering / replacement / refit | DESeq2-like outlier handling for the Wald path. |
| Basic additive formulas | Examples: `~ condition`, `~ batch + condition`. |
| Two-way interactions | Examples: `~ A + B + A:B`, `~ A * B`. |
| Precomputed design matrix | Covers advanced designs outside the built-in formula parser. |

## Additional Supported Outputs

These outputs are supported, but are not part of the Wald DEG test itself.

| DESeq2 feature | Compatibility note |
|---|---|
| VST | Variance-stabilized count output is available through `--vst` and `--write-vst-counts`. FlashDEG supports `--vst-fit-type parametric` and `--vst-fit-type mean`. |

## Supported with Caveats

Features in this section are available, but have documented compatibility
boundaries relative to DESeq2 R. These boundaries come from DESeq2 R features
that rely on external R packages or package-specific optimizer behavior.

| DESeq2 feature | Compatibility boundary |
|---|---|
| Dispersion `fitType="local"` | Local trend fitting uses FlashDEG's own local smoother, which is not an exact numerical replica of R's external `locfit` package, so the fitted dispersion trend can differ slightly. This local path is also entered automatically when the parametric fit fails to converge, so it can affect default `parametric` runs too. |
| `lfcShrink(type="apeglm")` | Optional post-processing. DESeq2 R delegates this path to the external `apeglm` package, while FlashDEG implements apeGLM-style shrinkage independently; difficult posterior shapes can therefore differ. |

## Unsupported Features

### Statistical Tests and Multiple Testing

| DESeq2 feature | Impact |
|---|---|
| `results(pAdjustMethod != "BH")` | FlashDEG always writes BH-adjusted p-values. DESeq2 R can request other `p.adjust` methods through `results(pAdjustMethod=...)`; those alternatives are not reproduced directly. |
| `results(filterFun=ihw)` | Independent Hypothesis Weighting (IHW) filtering is not available. Standard independent filtering remains available. |

### Normalization and Input Preparation

| DESeq2 feature | Impact |
|---|---|
| Size factors: `iterate` | This specific DESeq2 size-factor estimation method is not available. Standard ratio or poscounts normalization is usually enough for ordinary DEG calling. |
| External normalization inputs | User-supplied size factors and full normalization-factor matrices cannot be supplied directly. This matters when normalization must exactly reuse values computed outside FlashDEG. |
| Input preparation helpers | `DESeqDataSetFromHTSeqCount` and `collapseReplicates` are not part of FlashDEG. FlashDEG can read tximport-style estimated counts with `--tximport-round`, but it does not reproduce the `countsFromAbundance="no"` length-offset path. |

### Dispersion Fitting

| DESeq2 feature | Impact |
|---|---|
| Dispersion `fitType="glmGamPoi"` | glmGamPoi-specific dispersion fitting is not available. Standard DESeq2-style parametric and mean trends do not require it. |

### Advanced Design and Formula Handling

FlashDEG supports common DEG design formulas such as `~ condition`,
`~ batch + condition`, and two-way interactions such as `~ A + B + A:B`.

The formula parser does not reproduce the full R formula language. The
following should be prepared as a precomputed design matrix when needed:

- three-way and higher interactions, such as `~ A * B * C`,
- spline terms, such as `~ splines::ns(time, df=4)`,
- inline transformations, such as `~ log(age) + condition`,
- custom contrast coding that depends on R's `model.matrix` behavior.

### Transformations and LFC Shrinkage

| DESeq2 feature | Impact |
|---|---|
| VST `fitType="local"` | Local-trend VST output is not available. Parametric and mean VST remain supported. This does not affect Wald DEG calling. |
| `rlog` / `rlogTransformation` | rlog-transformed matrices for QC or visualization are not available. DEG p-values and adjusted p-values do not require rlog. |
| `lfcShrink(type="normal")` | Normal-prior shrinkage output is not available. Unshrunken Wald DEG calling does not require it. |
| `lfcShrink(type="ashr")` | ashr shrinkage output is not available. Unshrunken Wald DEG calling does not require it. |
