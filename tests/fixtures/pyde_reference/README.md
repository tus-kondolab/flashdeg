# PyDESeq2 Reference Fixtures

This directory contains the subset of upstream PyDESeq2 test fixtures that
`tests/test_main.cpp` reads.

The source checkout `PyDESeq2/` is ignored by Git, so `flashdeg_tests` must use
these tracked copies instead. The directory name intentionally avoids
`pydeseq2/`, because Git ignore matching is case-insensitive on the Windows
checkout and `.gitignore` ignores `PyDESeq2/`.

Upstream license: MIT. See `LICENSE.PyDESeq2`.
