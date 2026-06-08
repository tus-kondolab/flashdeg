# FlashDEG conda-forge recipe

This directory contains the conda-forge recipe files for FlashDEG.

The recipe intentionally uses the repository root `CMakeLists.txt`:

- `build.sh` builds from `${SRC_DIR}` on Linux and macOS.
- `bld.bat` builds from `%SRC_DIR%` on Windows.

The recipe uses FlashDEG's normal CMake install rule, so package layout is
controlled by `CMAKE_INSTALL_PREFIX` and `GNUInstallDirs`.

## Before submitting to conda-forge

Update `meta.yaml`:

1. Confirm the public source URL.
2. Replace the placeholder `sha256` with the release tarball hash.
3. Confirm the `recipe-maintainers` list.

For a GitHub release tarball:

```bash
curl -L -o flashdeg-v1.0.0.tar.gz \
  https://github.com/tus-kondolab/flashdeg/archive/refs/tags/v1.0.0.tar.gz
sha256sum flashdeg-v1.0.0.tar.gz
```

## Local recipe check

After filling the source hash, build locally with:

```bash
conda mambabuild conda-recipe
```

or:

```bash
conda build conda-recipe
```

For the actual conda-forge feedstock, copy `meta.yaml`, `build.sh`, and
`bld.bat` into the feedstock's `recipe/` directory.
