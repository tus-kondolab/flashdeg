# FlashDEG MSI installer

This directory contains WiX authoring for the FlashDEG Windows MSI installer.
The MSI installs the release package under:

```text
C:\Program Files\FlashDEG\
  flashdeg.exe
  openblas.dll
  README.md
  LICENSE
  THIRD_PARTY_NOTICES.md
  LICENSES\
```

`C:\Program Files\FlashDEG` is added to the system PATH by the MSI. This
standalone installer owns only the FlashDEG install directory and its own PATH
entry.

Build the release package folder first, then build the MSI:

```powershell
wix extension add -g WixToolset.UI.wixext/7.0.0
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\installer\Build-FlashdegMsi.ps1
```

WiX Toolset v7 requires accepting the WiX OSMF EULA before building. If it has
not already been accepted:

```powershell
wix eula accept wix7
```

or run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\installer\Build-FlashdegMsi.ps1 -AcceptWixEula
```

The default input package folder is:

```text
dist\flashdeg-1.0.0-windows-x86_64
```

The default output is:

```text
dist\flashdeg-1.0.0-windows-x86_64.msi
```

Validate the MSI without installing:

```powershell
wix msi validate .\dist\flashdeg-1.0.0-windows-x86_64.msi
```

Record the MSI SHA256 before publishing:

```powershell
Get-FileHash -Algorithm SHA256 .\dist\flashdeg-1.0.0-windows-x86_64.msi
```
