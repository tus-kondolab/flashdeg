# SciPy L-BFGS-B Vendor Copy

This directory contains the C translation of L-BFGS-B used by SciPy's
`scipy.optimize.__lbfgsb` module.

Imported files:

- `__lbfgsb.c`
- `__lbfgsb.h`
- `lapack_shims.cpp` (local compatibility shim)

The C source is recorded as the SciPy 1.17.1 L-BFGS-B C translation; the local
FlashDEG oracle environment also reports SciPy 1.17.1. See `VERSION.txt` for
provenance limitations and hashes.

## Local Changes

The original SciPy header combines the low-level `setulb` declaration with the
Python/Numpy extension-module wrapper. FlashDEG does not build a Python module,
so the vendored header was reduced to:

- BLAS/LAPACK Fortran ABI declarations used by `__lbfgsb.c`;
- an externally linked `setulb` declaration;
- no `Python.h` or `numpy/arrayobject.h` dependency.

The C file is intentionally kept unmodified.

Some OpenBLAS distributions, including the current Windows vcpkg package used
for FlashDEG development, expose BLAS symbols but not the two LAPACK symbols
that SciPy's L-BFGS-B C source calls directly. `lapack_shims.cpp` provides
small Fortran-ABI implementations of `dpotrf_` and `dtrtrs_` for the tiny
positive-definite systems used internally by L-BFGS-B.

CMake links the real system LAPACK symbols when the selected BLAS/LAPACK
backend exposes both `dpotrf_` and `dtrtrs_`. The local shims are compiled into
`scipy_lbfgsb` only when either symbol is missing.

## Updating

1. Copy `scipy/optimize/__lbfgsb.c` and `scipy/optimize/__lbfgsb.h` from the
   target SciPy version.
2. Reapply the local header change described above, or regenerate an equivalent
   minimal header.
3. Update `VERSION.txt` with the SciPy version, source location, date, and hashes.
4. Run the BLAS Fortran symbol test and the full FlashDEG test suite.
5. Run the PyDESeq2 oracle harness before replacing the previous vendor copy.
