# Third-Party Notices

This notice describes third-party material relevant to FlashDEG source and
binary releases. FlashDEG-owned code is distributed under the MIT License; see
the top-level `LICENSE`.

Release artifacts should include:

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `LICENSES/`

## Bundled Third-Party Code

### SciPy L-BFGS-B

FlashDEG vendors a copy of SciPy's C translation of L-BFGS-B under
`third_party/scipy_lbfgsb/`.

- License: BSD-3-Clause
- License file: `LICENSES/scipy-lbfgsb/LICENSE.txt`
- Source metadata: `LICENSES/scipy-lbfgsb/SOURCE.txt`

SciPy's C translation is derived from the original Fortran L-BFGS-B 3.0
implementation by Ciyou Zhu, Richard Byrd, Jorge Nocedal, and Jose Luis
Morales. The local FlashDEG header removes SciPy's Python/Numpy wrapper and
exports the low-level `setulb` routine for direct C/C++ use.

`third_party/scipy_lbfgsb/lapack_shims.cpp` is FlashDEG-owned glue code, not
SciPy code. It is used only when the selected BLAS/LAPACK backend does not
provide the LAPACK Fortran ABI symbols required by the vendored L-BFGS-B code.

## External Build And Runtime Dependencies

FlashDEG release builds normally use the following external dependencies. These
projects are not vendored in this repository unless a binary artifact
explicitly bundles their shared libraries.

### Eigen

FlashDEG uses Eigen for dense linear algebra when `FLASHDEG_USE_EIGEN=ON`.

- License: MPL-2.0, with some files under more permissive licenses
- License file: `LICENSES/eigen/LICENSE.txt`

### Boost.Math

FlashDEG uses Boost.Math for special functions and statistical distributions
when `FLASHDEG_USE_BOOST_MATH=ON`.

- License: Boost Software License 1.0
- License file: `LICENSES/boost/LICENSE.txt`

### BLAS / LAPACK / OpenBLAS

FlashDEG can link to OpenBLAS or another BLAS/LAPACK implementation when
`FLASHDEG_USE_BLAS=ON`.

- OpenBLAS license: BSD-3-Clause
- OpenBLAS license file: `LICENSES/openblas/LICENSE.txt`
- Reference LAPACK license file: `LICENSES/blas-lapack/LICENSE.txt`

If a binary release bundles an OpenBLAS, BLAS, LAPACK, Accelerate, MKL, or other
numerical shared library, include the exact license files supplied by that
binary dependency package.

## Source-Repository Test Fixtures

The public source repository can contain small test fixtures that are not part
of the user-facing FlashDEG binary.

### PyDESeq2 Port / Reimplementation

FlashDEG's early statistical core and `pydeseq2_*` implementation files were
written as a C++ port/reimplementation of PyDESeq2's DESeq2-like workflow.
PyDESeq2 is MIT-licensed; see `LICENSES/pydeseq2/LICENSE.txt`.

PyDESeq2 itself is not bundled in FlashDEG release binaries.

### PyDESeq2 Fixture Subset

`tests/fixtures/pyde_reference/` contains a small copied subset of PyDESeq2 CSV
test fixtures.

- Upstream project: PyDESeq2
- License: MIT
- License files:
  - `LICENSES/pydeseq2/LICENSE.txt`
- Source metadata: `LICENSES/pydeseq2/SOURCE.txt`
