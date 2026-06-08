# Beginner Guide To Design And Contrast

This guide explains the most common FlashDEG options for users who are new to
statistical models or command-line tools.

The short version is:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ condition" \
  --ref-level "condition=control" \
  --contrast "condition" "treated" "control" \
  --out results.csv
```

This means:

```text
Compare treated samples against control samples.
Report log2(treated/control).
```

## 1. The Metadata File

The metadata file describes each sample.

Example:

```csv
sample,condition,batch
s1,control,batch1
s2,control,batch1
s3,treated,batch1
s4,treated,batch2
s5,control,batch2
s6,treated,batch2
```

In this file:

- `condition` is a metadata column.
- `batch` is another metadata column.
- `control` and `treated` are groups inside the `condition` column.
- `batch1` and `batch2` are groups inside the `batch` column.

## 2. What `--design` Means

`--design` tells FlashDEG which metadata columns should be used by the model.

For a simple treated-vs-control experiment:

```bash
--design "~ condition"
```

This means:

```text
Use the condition column to explain expression differences.
```

### Why Is There A `~` Symbol?

The `~` symbol comes from the R/DESeq2 formula syntax. It separates:

```text
thing being explained ~ columns used to explain it
```

In an RNA-seq differential expression analysis, the thing being explained is
always gene expression. Because that left side is already known, FlashDEG only
asks you to write the right side:

```text
~ condition
```

Read this as:

```text
Model gene expression using the condition column.
```

So `~` is mostly a marker that says:

```text
the design starts here
```

It is kept because DESeq2 R uses the same syntax:

```r
design = ~ condition
```

In other words, FlashDEG writes:

```bash
--design "~ condition"
```

so that the design looks the same as the corresponding DESeq2 R design.

If samples were processed in different batches, use:

```bash
--design "~ batch + condition"
```

This means:

```text
Adjust for batch, then compare condition groups.
```

For most users, this is the key rule:

```text
Put columns you want to adjust for in --design.
Put the final biological comparison in --contrast.
```

## 3. What `--contrast` Means

`--contrast` tells FlashDEG which two groups should be compared in the final
result table.

Format:

```bash
--contrast <metadata-column> <test-group> <reference-group>
```

Example:

```bash
--contrast "condition" "treated" "control"
```

This means:

```text
Use the condition column.
Compare treated against control.
Report log2(treated/control).
```

Interpretation:

```text
log2FoldChange > 0  treated is higher than control
log2FoldChange < 0  treated is lower than control
```

## 4. What `--ref-level` Means

`--ref-level` chooses the reference group for a metadata column.

Example:

```bash
--ref-level "condition=control"
```

This means:

```text
Use control as the reference group for the condition column.
```

`--contrast` chooses what is reported in the result table. `--ref-level`
chooses how the design matrix is parameterized internally. These are related,
but they are not the same operation.

For a typical two-group comparison, it is usually best to make them agree:

```bash
--ref-level "condition=control" \
--contrast "condition" "treated" "control"
```

This reports `log2(treated/control)` and builds the design matrix with
`control` as the reference group.

If you omit `--ref-level`, FlashDEG uses its default factor-level ordering when
building the design matrix. The requested comparison is still
`treated/control`, but the internal coefficient parameterization can differ.
In exact mathematics these parameterizations describe the same comparison; in
finite-precision numerical fitting, very extreme genes may follow slightly
different convergence paths.

You can repeat `--ref-level` for multiple metadata columns:

```bash
--ref-level "condition=control" \
--ref-level "batch=batch1"
```

This means:

```text
condition reference group = control
batch reference group     = batch1
```

## 5. Two-Group Example

Metadata:

```csv
sample,condition
s1,CNTL
s2,CNTL
s3,CNTL
s4,TREATED
s5,TREATED
s6,TREATED
```

Command:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ condition" \
  --ref-level "condition=CNTL" \
  --contrast "condition" "TREATED" "CNTL" \
  --out treated_vs_cntl.csv
```

This reports:

```text
log2(TREATED/CNTL)
```

## 6. Batch-Adjusted Example

Metadata:

```csv
sample,condition,batch
s1,CNTL,batch1
s2,CNTL,batch2
s3,TREATED,batch1
s4,TREATED,batch2
```

Command:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ batch + condition" \
  --ref-level "condition=CNTL" \
  --ref-level "batch=batch1" \
  --contrast "condition" "TREATED" "CNTL" \
  --out treated_vs_cntl.csv
```

This reports:

```text
log2(TREATED/CNTL), adjusted for batch
```

## 7. More Than Two Groups

Suppose the metadata has:

```text
condition = control, breast, ovary, lung
```

To compare each disease group against control, run one command per comparison:

```bash
--design "~ condition" \
--ref-level "condition=control" \
--contrast "condition" "breast" "control" \
--out breast_vs_control.csv
```

```bash
--design "~ condition" \
--ref-level "condition=control" \
--contrast "condition" "ovary" "control" \
--out ovary_vs_control.csv
```

```bash
--design "~ condition" \
--ref-level "condition=control" \
--contrast "condition" "lung" "control" \
--out lung_vs_control.csv
```

You can also compare two non-control groups:

```bash
--contrast "condition" "breast" "ovary"
```

This reports:

```text
log2(breast/ovary)
```

## 8. Cancer Versus Control As One Group

If you want to compare all cancer samples together against control, create a
new metadata column.

Example:

```csv
sample,condition,cancer_status
s1,control,control
s2,breast,cancer
s3,ovary,cancer
s4,lung,cancer
```

Then run:

```bash
--design "~ cancer_status" \
--ref-level "cancer_status=control" \
--contrast "cancer_status" "cancer" "control"
```

This reports:

```text
log2(cancer/control)
```

Do not put both `condition` and `cancer_status` into the same design unless you
know the model is still valid. If `cancer_status` is completely determined by
`condition`, the design can become redundant.

## 9. Testing An Interaction Effect

An interaction asks whether the effect of one factor changes depending on
another factor. A common 2x2 example is genotype by treatment:

```csv
sample,genotype,treatment
s1,WT,ctrl
s2,WT,drug
s3,KO,ctrl
s4,KO,drug
```

Use both main effects plus the interaction:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ genotype + treatment + genotype:treatment" \
  --ref-level "genotype=WT" \
  --ref-level "treatment=ctrl" \
  --contrast-name "genotype[T.KO]:treatment[T.drug]" \
  --out interaction.csv
```

The shorter formula is equivalent:

```bash
--design "~ genotype * treatment"
```

The main-effect columns answer simpler questions:

```text
genotype[T.KO]        KO vs WT when treatment is ctrl
treatment[T.drug]    drug vs ctrl when genotype is WT
```

The interaction column answers the extra question:

```text
genotype[T.KO]:treatment[T.drug]
```

Read this as:

```text
Does drug change KO differently than it changes WT, beyond the two main effects?
```

If you are unsure about the exact column name, write the design matrix:

```bash
--write-design-matrix design.csv
```

Then choose the interaction column from `design.csv` and pass it to
`--contrast-name`. FlashDEG uses treatment-contrast column names such as
`A[T.level]:B[T.level]`.

## 10. Advanced: Precomputed Design Matrices

Most users should skip this section.

Use `--design-matrix` only if you have already created the model matrix
yourself, for example in R or Python.

When you use `--design-matrix`, FlashDEG no longer knows which column means
`condition`, `batch`, or an interaction term. You must tell it which model
columns to add or subtract.

Example design-matrix columns:

```text
Intercept, condition[T.treated]
```

To test `treated` vs `control`, use:

```bash
--contrast-vector 0,1
```

This means:

```text
Ignore Intercept.
Use condition[T.treated].
```

Another example:

```text
Intercept, condition[T.B], condition[T.C]
```

To test `C` vs `B`, use:

```bash
--contrast-vector 0,-1,1
```

This means:

```text
Ignore Intercept.
Subtract condition[T.B].
Add condition[T.C].
```

Again, ordinary users should prefer `--design` and `--contrast`.
