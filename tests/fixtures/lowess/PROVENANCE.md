# Independent-filtering lowess fixtures

Two `theta,numRej` curves used by `test_lowess_r_compat_selection` in
`tests/test_main.cpp` to lock in the DESeq2-R-compatible independent-filtering
theta selection (`pydeseq2::utils::lowess_r_compat`).

Each curve is the 50-point independent-filtering grid reconstructed from a real
benchmark result CSV (baseMean + pvalue columns), replaying DESeq2 R's
`pvalueAdjustment` at `alpha = 0.1`:

- `lowerQuantile = mean(baseMean == 0)`, `upperQuantile = 0.95`
- `theta = linspace(lowerQuantile, 0.95, 50)`
- `cutoff = quantile(baseMean, theta, type = 7)`
- `numRej(cutoff) = sum( BH(pvalue[baseMean >= cutoff]) < alpha )`

The numRej curve is identical whichever tool produced the result CSV, because
baseMean and pvalue agree across FlashDEG / DESeq2 R; only the downstream
theta-selection (lowess + RMSE threshold) differs, which is what these
fixtures pin.

| File | Source dataset | DESeq2 R selected theta index |
|------|----------------|-------------------------------|
| `tcga_brca_numrej.csv` | TCGA-BRCA, 64k genes x 1256 samples | 0 |
| `large4500_numrej.csv` | GTEx-like adipose vs brain, 74,628 genes x 4,535 samples | 2 |

The expected indices were obtained from R native `stats::lowess(theta, numRej,
f = 1/5)` + DESeq2's selection rule, cross-checked against a faithful port of
R's clowess/lowest (now shipped in C++ as `pydeseq2::utils::lowess_r_compat`),
which matched R native bit-for-bit on maxFit / rmse / threshold for both curves.

The GTEx curve is the regression case: the earlier PyDESeq2-derived lowess
(iter=4 + r_compat bandwidth) blew up to maxFit = 65536 (= 2^16) on its steep
post-peak descent and selected index 0 (no filtering), inflating padj by the
BH-denominator ratio ~1.0403 and dropping ~54-67 borderline DEGs. lowess_r_compat
reproduces R's index 2.

To regenerate: from a result CSV with `baseMean` and `pvalue` columns, replay
the four-line `pvalueAdjustment` recipe above at `alpha = 0.1` (numpy
`quantile(..., method="linear")` matches R `type = 7`; BH must ignore NaN
p-values, matching R `p.adjust`), then keep the `theta,numRej` columns.
