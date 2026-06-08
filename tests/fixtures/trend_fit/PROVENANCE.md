# Parametric dispersion trend-fit regression fixture

`smoke_active_set.csv` is the 9,931-gene active set (`gene_id, baseMean,
genewiseDispersion`) used by `test_parametric_trend_fit_step_halving` in
`tests/test_main.cpp` to lock in the R-style step-halving safeguard inside
`fit_gamma_glm_identity_irls` (in `src/core/pydeseq2_dds.cpp`).

## Source

Generated from the 10,000-gene synthetic dataset at
`tests/smoke/results/smoke_counts.csv` + `smoke_metadata.csv` (12 samples,
6 control vs 6 treated; deterministic arithmetic, no RNG). The counts CSV
is regenerated on every run by `tests/smoke/run_deseq2_compare.sh`, so the
fixture is deterministic in CI.

Steps:

1. Run FlashDEG dev build with `--write-genewise-dispersions` and
   `--write-base-means` to dump per-gene Cox-Reid MLE and baseMean.
2. Filter to the active set FlashDEG's `fit_parametric_dispersion_trend`
   uses in `--compat-mode deseq2-r`:
   `baseMean > 0 && isfinite(baseMean) && isfinite(genewise) &&
    genewise > 100 * min_disp` (with `min_disp = 1e-8`).
3. Write `gene_id, baseMean, genewiseDispersion` with `%.6g` formatting.

## Expected fit (R native)

R's `glm(disp ~ I(1/baseMean), family = Gamma(link = "identity"),
        start = c(0.1, 1))` on this active set converges in 4 outer
iterations to `(intercept = 0.002141, slope = 2.853513)`, i.e.

```text
alpha(baseMean) = 0.002141 + 2.853513 / baseMean
```

These coefficients correspond to `DispersionTrendFit::a0` and `::a1` in
FlashDEG's struct: `a0` is the intercept (constant term), `a1` is the
1/baseMean coefficient.

## Why this fixture matters

Pre-fix, FlashDEG's `fit_gamma_glm_identity_irls` returned without
converging on the very first IRLS step because the unconstrained weighted
least squares step produced `(a0, a1) = (-0.022, 6.41)` (a0 negative).
The old code path bailed via `if (next[0] <= 0 || ...) return fit;`, so
`fit_parametric_dispersion_trend` silently fell back to local trend and
diverged from DESeq2 R on per-gene dispersions by 10-30%, flipping 16
borderline DEG calls (10 up + 6 down) on the 10,000-gene smoke dataset.

The unit test asserts:

- `kind == DispersionTrendKind::parametric` (no local fallback)
- `converged == true`
- `0 < a0 < 0.01` (R native: 0.002141)
- `2.5 < a1 < 3.2` (R native: 2.854)

The slack tolerates IRLS convergence-criterion differences across
platforms (FlashDEG uses log-coef change; R uses deviance change).

## Regenerate

```bash
# Re-run the dev build with full dispersion dumps:
build-wsl-dev/flashdeg run \
  --counts tests/smoke/results/smoke_counts.csv \
  --metadata tests/smoke/results/smoke_metadata.csv \
  --design "~ condition" --compat-mode deseq2-r \
  --fit-type parametric --size-factors ratio \
  --ref-level condition=control --contrast condition treated control \
  --threads 1 --quiet \
  --out /tmp/smoke_out.csv \
  --write-genewise-dispersions /tmp/genewise.csv \
  --write-base-means /tmp/basemean.csv

# Then filter and write the active set CSV via the steps above.
```
