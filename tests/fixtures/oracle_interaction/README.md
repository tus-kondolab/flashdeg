# Two-Way Interaction Oracle Fixture

This fixture stores a saved DESeq2 R oracle for a synthetic genotype by
treatment interaction dataset.

CTest does not run R for this oracle. It runs FlashDEG against the saved counts
and metadata, writes the FlashDEG result into the build tree, and compares that
result to `oracle_interaction_deseq2_full.csv` with
`tools/oracle_interaction_diff.py`.

Regenerate only when intentionally updating the oracle snapshot:

```bash
bash tools/regenerate_oracle_interaction.sh
```
