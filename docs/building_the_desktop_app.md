# Building the FlashDEG GUI from Source

This guide builds the FlashDEG desktop application executable (`flashdeg-gui`)
from a fresh `git clone`, **assuming you have no development tools installed
yet**. It covers **macOS, Linux, and Windows** separately — follow the one
section for your operating system top to bottom.

Following your platform's steps produces the GUI executable in the repository's
`dist/` folder:

| OS | Executable produced in `dist/` |
| --- | --- |
| macOS | `dist/flashdeg-gui` |
| Linux | `dist/flashdeg-gui` |
| Windows | `dist/flashdeg-gui.exe` |

> This guide covers **only** building the GUI executable. The `flashdeg`
> command-line engine is built separately (see the top-level
> [`README.md`](../README.md)).

> **macOS note:** the table above is the *bare* executable (handy for quick
> testing, but double-clicking it in Finder opens a Terminal window). For a
> normal, double-clickable application, build the **`FlashDEG.app`** bundle and
> add the engine to it — the macOS section below covers this end to end.

Each platform needs two toolchains — **Node.js** (web frontend) and **Rust**
(native app) — plus an OS-specific webview library and **Git**. Every section
below installs all of them.

---

# macOS

Use the **Terminal** app for every command.

## M1. Install the development tools

1. **Apple command-line tools** (gives you `git` and a C compiler):

   ```bash
   xcode-select --install
   ```

2. **Homebrew** (package manager) — skip if you already have it:

   ```bash
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

3. **Node.js**:

   ```bash
   brew install node
   ```

4. **Rust**:

   ```bash
   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
   source "$HOME/.cargo/env"
   rustup default stable
   ```

macOS already includes **WebKit**, which Tauri uses to render the GUI — no extra
webview install is needed.

Verify everything is available:

```bash
git --version && node --version && npm --version && rustc --version && cargo --version
```

## M2. Get the source code

`xcode-select --install` above gave you `git`, so clone the repository:

```bash
git clone <REPOSITORY_URL> flashdeg
cd flashdeg
```

The GUI lives in the `gui_app/` subfolder.

## M3. Build the application bundle (`FlashDEG.app`)

```bash
cd gui_app
npm install                # frontend deps + the Tauri CLI
npm run tauri:build        # IMPORTANT: do NOT pass --no-bundle
```

Building **without** `--no-bundle` produces a real macOS app bundle. Look under
`gui_app/src-tauri/target/release/`:

```text
bundle/macos/FlashDEG.app/               ← the application (use this) — a FOLDER
bundle/dmg/FlashDEG_0.0.1_<arch>.dmg     ← optional disk-image installer
flashdeg-gui                             ← bare executable (NOT for distribution)
```

`<arch>` is `aarch64` on Apple Silicon, `x64` on Intel. Double-clicking
`FlashDEG.app` opens the GUI with no Terminal window.

> **`FlashDEG.app` is a folder**, not a single file — a macOS *application
> bundle*. Finder shows it as one app icon; in Terminal it is a directory
> (`ls FlashDEG.app/Contents/MacOS`). That is why steps below copy files *into*
> it, and why archiving it needs a bundle-aware tool (Finder's “Compress”, or
> `ditto`) to preserve its contents and permissions.

> **Quick dev runs** (no bundle needed): `npm run tauri:dev` for live reload,
> or `npm run tauri:build -- --no-bundle` for just the bare `flashdeg-gui`.
> Note: double-clicking the bare executable in Finder launches it through
> Terminal — that is expected, and is why distribution uses the `.app`.

## M4. Add the `flashdeg` engine to the app

`FlashDEG.app` contains only the GUI. It needs the separate `flashdeg`
command-line engine (build it per the top-level
[`README.md`](../README.md)). At launch the GUI looks for `flashdeg` in this
order: **(1) next to its own executable, (2) on `PATH`, (3) in the folder that
contains the `.app`.** So pick one of these:

**Option A — inside the bundle (self-contained, recommended):**

```bash
cp /path/to/flashdeg "FlashDEG.app/Contents/MacOS/flashdeg"
# If your flashdeg build links a DYNAMIC OpenBLAS, copy its library beside it:
# cp /path/to/libopenblas.dylib "FlashDEG.app/Contents/MacOS/"
```

Everything ships inside `FlashDEG.app`; the GUI finds the engine automatically
(it sits next to the GUI executable in `Contents/MacOS/`).

**Option B — beside the app (one folder, like the Windows layout):**

```text
FlashDEG/
  FlashDEG.app/         # a folder (the app bundle)
  flashdeg
  libopenblas.dylib    # only if your build uses a dynamic OpenBLAS
```

The GUI also finds a `flashdeg` placed next to `FlashDEG.app`.

> **Does it need `libopenblas.dylib`?** Only if your macOS `flashdeg` was built
> against a dynamic OpenBLAS. If it links Apple's **Accelerate** framework for
> BLAS/LAPACK (a system framework), the single `flashdeg` binary is enough.

You can always skip bundling and point the GUI at any `flashdeg` by hand using
the binary picker in the Analyze step.

## M5. First launch (Gatekeeper)

The app is **not code-signed**, so macOS blocks it on first open ("…cannot be
opened because it is from an unidentified developer"). Clear it once, either
way:

- **Right-click `FlashDEG.app` → Open**, then confirm in the dialog; or
- remove the quarantine attribute from Terminal:

  ```bash
  xattr -dr com.apple.quarantine "FlashDEG.app"
  ```

After this the app opens normally on every launch.

---

# Linux

Instructions use **Debian/Ubuntu** (`apt`). For Fedora use `dnf`, for Arch use
`pacman` — the package names map closely.

## L1. Install the development tools

1. **Base tools + Tauri's webview dependencies**:

   ```bash
   sudo apt update
   sudo apt install -y \
     git curl wget file build-essential \
     libwebkit2gtk-4.1-dev libxdo-dev libssl-dev \
     libayatana-appindicator3-dev librsvg2-dev
   ```

   `libwebkit2gtk-4.1-dev` and the packages after it are what Tauri needs to
   render and compile the GUI on Linux.

2. **Node.js** (LTS). The official NodeSource setup works across distros (or use
   your distro's package if it is Node 18+):

   ```bash
   curl -fsSL https://deb.nodesource.com/setup_lts.x | sudo -E bash -
   sudo apt install -y nodejs
   ```

3. **Rust**:

   ```bash
   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
   source "$HOME/.cargo/env"
   rustup default stable
   ```

Verify:

```bash
git --version && node --version && npm --version && rustc --version && cargo --version
```

## L2. Get the source code

`git` was installed in L1, so clone the repository:

```bash
git clone <REPOSITORY_URL> flashdeg
cd flashdeg
```

The GUI lives in the `gui_app/` subfolder.

## L3. Build the GUI executable

```bash
cd gui_app
npm install
npm run tauri:build -- --no-bundle
```

This produces `gui_app/src-tauri/target/release/flashdeg-gui`.

## L4. Copy it into `dist/`

```bash
mkdir -p ../dist
cp src-tauri/target/release/flashdeg-gui ../dist/
```

You now have `dist/flashdeg-gui`.

> To run the app while developing use `npm run tauri:dev` from `gui_app/`.

---

# Windows

Use **PowerShell** for every command.

## W1. Install the development tools

`winget` ships with Windows 10/11. Run each line and accept any prompts:

```powershell
winget install --id Git.Git -e
winget install --id OpenJS.NodeJS.LTS -e          # Node.js + npm
winget install --id Rustlang.Rustup -e            # Rust toolchain installer
winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
  --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --quiet --wait"
```

Then finish the Rust setup (installs the MSVC target the app needs):

```powershell
rustup default stable
```

**Close and reopen PowerShell** so the new `PATH` entries take effect.

Notes:
- **WebView2** (the engine that renders the GUI) is preinstalled on Windows 11
  and recent Windows 10; no manual step is needed.
- You do **not** need to install Tauri by hand — the Tauri CLI is pulled in by
  `npm install` below.

Verify:

```powershell
git --version; node --version; npm --version; rustc --version; cargo --version
```

## W2. Get the source code

`winget install Git.Git` above gave you `git`, so clone the repository:

```powershell
git clone <REPOSITORY_URL> flashdeg
cd flashdeg
```

The GUI lives in the `gui_app\` subfolder.

## W3. Build the GUI executable

```powershell
cd gui_app
npm install
npm run tauri:build -- --no-bundle
```

This produces `gui_app\src-tauri\target\release\flashdeg-gui.exe`.

## W4. Assemble the distribution folder

Build and deploy are **separate** steps, so building never touches the
distribution folder on its own.

**Build** the release exe (output stays in `target\release\`, nothing copied):

```powershell
npm run build:win          # = tauri build --no-bundle
```

**Deploy** — copy the built GUI files into `dist\FlashDEG\`:

```powershell
npm run deploy:win         # asks before overwriting existing GUI files
npm run deploy:win -- -Force   # overwrite without asking
```

This produces (under the repository root):

```text
dist\FlashDEG\
  flashdeg-gui.exe
  gene_maps\
    gene_symbols_human.tsv.gz
    gene_symbols_fly.tsv.gz
```

`deploy:win` updates **only** `flashdeg-gui.exe` and `gene_maps\` — it never
wipes the folder, so anything you added by hand is kept. `gene_maps\` holds the
external gene-symbol tables (see the "Gene-symbol display maps" section). It
does **not** bundle the `flashdeg` engine — add it (and `openblas.dll` if your
build links OpenBLAS dynamically) to the same folder yourself:

```powershell
Copy-Item <build>\flashdeg.exe   dist\FlashDEG\
Copy-Item <build>\openblas.dll   dist\FlashDEG\   # only if dynamically linked
```

The GUI finds `flashdeg.exe` next to itself at launch.

> To run the app while developing use `npm run tauri:dev` from `gui_app\`.

---

## Gene-symbol display maps (optional, all platforms)

The results view can show gene **symbols** instead of raw IDs (Ensembl `ENSG…`
for human, FlyBase `FBgn…` for *Drosophila*). The lookup tables are **not**
embedded in the executable — they are plain external files the GUI reads at
launch from a **`gene_maps/` folder next to the GUI executable**, so you can
replace or extend them by hand:

```text
<app folder>/
  flashdeg-gui(.exe)        # or FlashDEG.app on macOS
  gene_maps/
    gene_symbols_human.tsv.gz
    gene_symbols_fly.tsv.gz
```

The repository ships these under [`gui_app/gene_maps/`](../gui_app/gene_maps/);
copy that folder next to the GUI executable when distributing. They are
regenerated from Ensembl BioMart with
`gui_app/scripts/fetch-gene-symbols.ps1 -Species all`. Each file is a
gzip-compressed TSV of `gene_id<TAB>symbol` lines. If the folder (or a species
file) is missing, the symbol toggle simply falls back to showing raw gene IDs —
the app still runs.

> On macOS the GUI executable lives at `FlashDEG.app/Contents/MacOS/`, so the
> co-located layout is `FlashDEG.app/Contents/MacOS/gene_maps/`.

## Troubleshooting (all platforms)

- **First build is slow.** Compiling the Rust/Tauri backend from scratch takes
  several minutes. Later builds are incremental and much faster.
- **The GUI starts but cannot run an analysis.** The GUI needs the separate
  `flashdeg` command-line engine; building and providing it is covered
  elsewhere and is out of scope for this guide.
- **Port 1420 already in use (dev mode).** The Vite dev server uses port 1420.
  Stop whatever else is using it, or change `build.devUrl` in
  `gui_app/src-tauri/tauri.conf.json`.
- **Windows: linker / `link.exe` errors from Rust.** The Visual Studio C++
  workload didn't install. Re-run the `Microsoft.VisualStudio.2022.BuildTools`
  command in W1, or install "Desktop development with C++" from the Visual
  Studio Installer.

---

## Quick reference

```text
git clone <repo> flashdeg && cd flashdeg
cd gui_app && npm install

# Bare executable (quick test):
npm run tauri:build -- --no-bundle
macOS / Linux : mkdir -p ../dist && cp src-tauri/target/release/flashdeg-gui ../dist/
Windows       : Copy-Item src-tauri\target\release\flashdeg-gui.exe ..\dist\

# Windows distributable (recommended): build, then deploy to dist\FlashDEG\
npm run build:win                  # build release exe (no copy)
npm run deploy:win -- -Force       # copy GUI files into dist\FlashDEG\
#   then add the engine: Copy-Item <build>\flashdeg.exe dist\FlashDEG\

# macOS distributable app (recommended): build the bundle, then add the engine
npm run tauri:build
cp /path/to/flashdeg "src-tauri/target/release/bundle/macos/FlashDEG.app/Contents/MacOS/flashdeg"
xattr -dr com.apple.quarantine "src-tauri/target/release/bundle/macos/FlashDEG.app"
```
