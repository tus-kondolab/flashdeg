# PyDESeq2 Fixture Provenance

These CSV files are a minimal copied subset of the upstream PyDESeq2 test data
used by `flashdeg_tests`.

- Upstream project: PyDESeq2
- Upstream version in local reference tree: 0.5.4
- License: MIT, copyright Owkin and contributors
- Local source path used for sync: `PyDESeq2/`
- Sync date: 2026-05-27
- Upstream commit SHA: not available in the local ignored `PyDESeq2/` tree

To refresh this subset after updating the local PyDESeq2 reference checkout:

```bash
bash tools/copy_fixtures.sh
```

The sync script uses an explicit file list so unrelated upstream fixtures do not
enter the repository accidentally.
