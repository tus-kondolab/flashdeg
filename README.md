# FlashDEG

FlashDEG is a fast, lightweight implementation of the DESeq2 algorithm for
bulk RNA-seq differential expression analysis. It is designed for local
desktops, servers, and workflow systems where runtime, memory use, and
deployment size matter. In benchmarked workloads, FlashDEG can run up to 50x
faster than DESeq2 and use substantially less memory.

## Highlights

- **Fast**: up to 50x faster than DESeq2 in benchmarked workloads.
- **Memory efficient**: uses substantially less peak memory than DESeq2 in
  benchmarked workloads; in one approximately 58,000-gene x 17-sample run,
  FlashDEG used about 180 MB compared with about 1.1 GB for DESeq2.
- **Lightweight**: native executable deployment, under 2 MB for the binary
  alone.
- **Nearly exact DESeq2 emulation**: closely matches DESeq2 behavior for
  standard bulk RNA-seq workflows.
- **Cross-platform**: supports Windows/Mac/Linux. 

## Installation

### Pre-Built Binaries

Pre-built releases for Windows, macOS, and Linux are available on the GitHub
[Releases page](https://github.com/tus-kondolab/flashdeg/releases). If a
pre-built binary is not available for your platform, build from source using
one of the build paths below.

### Verify Installation

After building or installing, verify the executable:

```bash
flashdeg --version
```

The version command prints only the FlashDEG version. For build provenance,
including the git revision, build date, numerical backends, and build flags,
use:

```bash
flashdeg --build-info
```

Development builds with uncommitted changes append `-dirty` to the git revision
shown by `--build-info`.

## Quick Start

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ condition" \
  --ref-level "condition=control" \
  --contrast "condition" "treated" "control" \
  --out results.csv
```

`--ref-level` and `--contrast` are both standard — include them together
in every invocation. `--contrast` controls the report direction
(`log2(treated/control)`); `--ref-level` controls which level is the
design-matrix intercept. Omitting `--ref-level` falls back to alphabetical
factor ordering, which is a different (though mathematically equivalent)
parameterization and can shift extreme-asymmetry genes near the padj
boundary.

### Input Files
### Counts

Counts are provided as CSV with feature IDs and sample IDs. By default,
FlashDEG expects genes/features as rows and samples as columns:

`counts.csv`

```csv
gene_id,sample1,sample2,sample3,sample4,sample5,sample6
gene_001,10,12,9,28,31,27
gene_002,0,1,0,3,4,2
gene_003,55,48,52,50,47,53
gene_004,4,5,6,18,21,19
gene_005,120,115,130,62,58,65
:
```

If your file has samples as rows and genes/features as columns, use the
advanced `--features-as-cols` option.

### Metadata

Metadata must contain sample rows whose names match the count sample names.
Factors referenced in `--design`, `--contrast`, and `--ref-level` are read
from this table:

`metadata.csv`

```csv
sample_id,condition
sample1,control
sample2,control
sample3,control
sample4,treated
sample5,treated
sample6,treated
```

### Output
The main output CSV contains one row per gene:

```text
gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj
gene_001,19.47,1.69,0.42,4.02,5.80e-05,1.45e-04
gene_002,1.18,2.31,1.12,2.06,0.0394,0.0493
gene_003,50.98,-0.02,0.21,-0.10,0.919,
gene_004,10.12,2.29,0.55,4.16,3.16e-05,1.05e-04
gene_005,87.36,-1.03,0.31,-3.32,0.00090,0.00150
:
```

Empty `padj` values mean the gene did not receive an adjusted p-value, usually
because of independent filtering or Cook outlier filtering.

### LFC Shrinkage (optional)

To compute apeGLM-style shrunken LFC for a single coefficient:

```bash
flashdeg run \
  --counts counts.csv \
  --metadata metadata.csv \
  --design "~ condition" \
  --ref-level "condition=control" \
  --contrast "condition" "treated" "control" \
  --out results.csv \
  --lfc-shrink \
  --lfc-shrink-coef "condition[T.treated]" \
  --write-shrunken-lfc shrunken_lfc.csv
```

`--write-shrunken-lfc` writes plot-ready columns:

```text
gene_id,baseMean,shrunkenLog2FoldChange,shrunkenLfcSE,pvalue,padj,converged
```

Use the unshrunken Wald `pvalue` / `padj` for DEG calls. The shrunken LFC is
intended for effect-size visualization and ranking. FlashDEG emits warnings
for extreme shrinkage-prior settings and for suspicious cases where many
significant genes are shrunk close to zero.

## Status

FlashDEG is designed for common bulk RNA-seq differential expression analysis.
It reads a count matrix and sample metadata, compares groups, and reports log2
fold changes, p-values, and adjusted p-values. It can also write VST-normalized
values and shrunken LFC values for downstream visualization. For DESeq2 feature
compatibility, see [docs/deseq2_compatibility.md](docs/deseq2_compatibility.md).

Supported features include:

- count CSV validation and orientation handling
- design formulas such as `~ condition` and `~ batch + condition`
- group comparisons with `--contrast` and `--ref-level`
- ratio and poscounts size-factor normalization
- dispersion trend fitting with `parametric`, `mean`, and approximate `local`
- Wald-test results with BH-adjusted p-values
- Cook outlier filtering, replacement, and refit
- VST output
- apeGLM-style LFC shrinkage output

## Command-Line Options

Use `flashdeg run --help` to see the options accepted by your installed
binary. `flashdeg --help` shows the top-level commands, `flashdeg --version`
prints the version, and `flashdeg --build-info` prints build provenance that is
useful when reporting results. For the complete CLI reference, see
[docs/commands.md](docs/commands.md).

### Input And Output

| Option | Description |
|---|---|
| `--counts <path>` | Count CSV. Required. |
| `--metadata <path>` | Sample metadata CSV. Required. |
| `--out <path>` | Output path for the main result CSV. |

Note: FlashDEG never chooses default output paths. If `--out` is omitted, no
main Wald result CSV is written. Runs without `--out` only write files
explicitly requested with options such as `--write-vst-counts`,
`--write-shrunken-lfc`, or other `--write-*` options.

### Standard Design And Group Comparison

Use these options for ordinary RNA-seq differential expression analyses. This
is the recommended path for most users. See
[docs/beginner_contrast_guide.md](docs/beginner_contrast_guide.md) for a
beginner-friendly explanation.

| Option | Description |
|---|---|
| `--design <formula>` | Formula subset such as `"~ condition"` or `"~ batch + condition"`. |
| `--contrast <metadata-column> <test-group> <reference-group>` | Compare two groups from a metadata column. For example, `--contrast "condition" "treated" "control"` reports `log2(treated/control)`. |
| `--ref-level <metadata-column=reference-group>` | Reference group for the design-matrix intercept, e.g. `"condition=control"`. Repeat for multiple factors. **Include this flag in every standard invocation alongside `--contrast`** — they are orthogonal: `--contrast` picks the report direction (numerator / denominator of the Wald test), `--ref-level` picks the model-matrix parameterization. If omitted, FlashDEG falls back to alphabetical factor ordering (matching the DESeq2 R default for an unrelevelled `factor()`), which is a mathematically equivalent but numerically distinct parameterization — and at extreme-asymmetry boundary genes the fitted log2FC / pvalue can differ materially. |

For advanced options, output controls, profiling options, and the complete CLI
reference, see [docs/commands.md](docs/commands.md).

## Build

### Linux / WSL

Install the system dependencies, then use the Linux preset:

```bash
sudo apt-get install -y cmake ninja-build g++ libeigen3-dev libboost-math-dev libopenblas-dev
cmake --preset linux-system
cmake --build --preset linux-system -j
ctest --preset linux-system
```

For WSL/Linux oracle or debugging runs, use:

```bash
cmake --preset linux-dev-system
cmake --build --preset linux-dev-system -j
ctest --preset linux-dev-system
```

### macOS

For a release-oriented macOS build, install the numerical dependencies with
Homebrew and configure manually:

```bash
xcode-select --install  # only needed if Apple Clang is not installed yet
brew install cmake ninja eigen boost openblas

cmake -S . -B build-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFLASHDEG_BUILD_TESTS=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix eigen);$(brew --prefix boost);$(brew --prefix openblas)"

cmake --build build-macos
ctest --test-dir build-macos --output-on-failure
```

The numerical backend options (`FLASHDEG_USE_EIGEN`,
`FLASHDEG_USE_BOOST_MATH`, `FLASHDEG_USE_BLAS`, and
`FLASHDEG_USE_SCIPY_LBFGSB`) default to `ON`; pass them explicitly only when
overriding a stale CMake cache or making a developer-only fallback build.

If CMake cannot find Homebrew's OpenBLAS on your machine, use vcpkg instead.


```bash
git clone https://github.com/microsoft/vcpkg "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh"
export VCPKG_ROOT="$HOME/vcpkg"

"$VCPKG_ROOT/vcpkg" install eigen3 boost-math openblas
cmake -S . -B build-macos-vcpkg -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFLASHDEG_BUILD_TESTS=ON \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

cmake --build build-macos-vcpkg
ctest --test-dir build-macos-vcpkg --output-on-failure
```

### Windows With vcpkg

Get the FlashDEG source code:

```powershell
winget install --id Git.Git -e  # skip if git is already installed
cd "$env:USERPROFILE"
git clone https://github.com/tus-kondolab/flashdeg.git
cd flashdeg
```

Install the required command-line build tools and vcpkg dependencies:

```powershell
winget install --id Kitware.CMake -e
winget install --id Ninja-build.Ninja -e

cd "$env:SystemDrive\"
git clone https://github.com/microsoft/vcpkg.git vcpkg
.\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$env:SystemDrive\vcpkg"
& "$env:VCPKG_ROOT\vcpkg.exe" install eigen3 boost-math openblas
```

Open **Developer PowerShell for Visual Studio** so the MSVC compiler
environment is active, then configure, build, and test:

```powershell
cd "$env:USERPROFILE\flashdeg"
cmake --preset vcpkg-ninja
cmake --build --preset vcpkg-ninja
ctest --preset vcpkg-ninja --output-on-failure
```

If Visual Studio is not installed, install either **Visual Studio Build Tools**
or **Visual Studio Community** with the **Desktop development with C++**
workload, then rerun the build from Developer PowerShell.

Release-oriented builds hide and reject developer-only `--write-*` intermediate
outputs. Maintainers who need oracle/debug outputs can use the developer preset:

```powershell
cmake --preset dev-vcpkg-ninja
cmake --build --preset dev-vcpkg-ninja
ctest --preset dev-vcpkg-ninja --output-on-failure
```

Use the developer-only fallback below only for FlashDEG development or CI
portability checks; it is not a public release build.

### Developer-Only Fallback Build

The dependency-free `no-deps` build is for maintainers and CI portability
testing. It is not a public release target and should not be used for published
analysis results.

## Numerical Backends

FlashDEG uses external numerical libraries for reliable and portable numerical
calculation:

- Eigen for linear algebra
- OpenBLAS / BLAS / LAPACK for accelerated numerical kernels
- Boost.Math for special functions and statistical distributions
- a vendored BSD-licensed SciPy L-BFGS-B C translation for bounded optimization

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for license and provenance
details.

## Acknowledgments

FlashDEG implements statistical methods established by the following projects
and papers. Please cite the relevant upstream methods in publications that use
FlashDEG results:

- **DESeq2**: Love, M.I., Huber, W., and Anders, S. (2014). Moderated
  estimation of fold change and dispersion for RNA-seq data with DESeq2.
  *Genome Biology* 15:550.
- **apeGLM**: Zhu, A., Ibrahim, J.G., and Love, M.I. (2019). Heavy-tailed prior
  distributions for sequence count data. *Bioinformatics* 35:2084-2092.
- **PyDESeq2**: Muzellec, B. et al. (2023). PyDESeq2: a python package for bulk
  RNA-seq differential expression analysis. *Bioinformatics* 39:btad547.

See [CITATION.cff](CITATION.cff) for machine-readable citation metadata for
FlashDEG itself.

## License

FlashDEG-owned source code is distributed under the MIT License. See
[LICENSE](LICENSE).

Bundled and optional third-party components are documented in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Citation

Citation metadata is provided in [CITATION.cff](CITATION.cff).
