# Test Fixtures

This directory contains small, tracked data snapshots used by automated tests.

- `pyde_reference/`: a minimal subset of PyDESeq2 0.5.4 CSV fixtures used by
  `flashdeg_tests`.
- `oracle_interaction/`: a saved DESeq2 R result snapshot for the two-way
  interaction CTest. The CTest runner uses these CSVs directly and does not run
  R.

Do not point tests at ignored local reference checkouts such as `PyDESeq2/` or
generated output directories such as `tools/results/`.
