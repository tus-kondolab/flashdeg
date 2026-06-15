# FlashDEG GUI

Tauri 2 + React + TypeScript + Vite. [`gui_plan.md`](gui_plan.md) is the design
spec; [`gui_ux_improvements.md`](gui_ux_improvements.md) tracks the UX redesign.

## Workflow

The UI is a three-step workflow over an always-present project (an untitled
project is created on launch; every screen edits it as the single source of
truth — see gui_ux_improvements.md § 3):

1. **Data** — pick an existing counts+metadata pair, or build one from
   featureCounts files. Writes to `project.inputs`.
2. **Analyze** — edit design / contrast / options, check inputs (§ 6 / § 7
   validation, inline), and Run. Blockers disable Run.
3. **Results** — table + volcano + MA, with bidirectional selection sync.

The top **project bar** has New / Open / Save and a History panel; the FlashDEG
binary path and thread count are app-level settings (persisted to localStorage).

## Status

- [x] § 4 Project file v1 (serde schema, fixtures, frozen snapshot test)
- [x] § 15 plot library = **Plotly (scattergl)**
- [x] § 5.3 / § 8 Results: Open results.csv → Volcano + MA + sortable table
- [x] § 5.1 / § 5.2 Data: existing pair + featureCounts import
- [x] § 6 / § 7 Validation and guardrails (inline in Analyze)
- [x] § 9 / § 11 Run: spawn + live log + cancel + profile.json timing
- [x] § 12 Run history in the project file
- [ ] § 13 / § 14 Bundling and signing

### Results / schema compatibility

Results CSVs need FlashDEG-compatible columns
(`gene_id, baseMean, log2FoldChange, lfcSE, stat, pvalue, padj`); the InMoose
alias `adj_pvalue` is accepted, and missing values (`NA`, `NaN`, empty) are
preserved as `null`. Source-of-truth schemas are kept in lock-step between Rust
([src-tauri/src/results.rs](src-tauri/src/results.rs)) and TypeScript
([src/lib/results.ts](src/lib/results.ts)). The project → backend-request
mapping lives in [src/lib/requests.ts](src/lib/requests.ts).

## Prerequisites

- Node.js 20+
- Rust 1.77+
- Tauri 2 platform prerequisites:
  - Windows: WebView2 (bundled with Windows 11; install Evergreen Runtime on Win 10)
  - macOS: Xcode Command Line Tools
  - Linux: `libwebkit2gtk-4.1-dev`, `build-essential`, `libssl-dev`,
    `libgtk-3-dev`, `libayatana-appindicator3-dev`, `librsvg2-dev`

Icons are not yet committed. Before `tauri build` you must place icon files
at `src-tauri/icons/` matching the names in `tauri.conf.json`. Use
`npx @tauri-apps/cli icon path/to/source.png` to generate the set.

## Develop

```bash
cd gui_app
npm install
npm run tauri:dev
```

## Test (Rust backend only, no UI)

```bash
cd gui_app/src-tauri
cargo test
```

The integration tests in `tests/project_fixtures.rs` exercise each
`contrast.kind` variant and a frozen byte-equal snapshot of the
`factor_levels` shape. If a serde change drifts the wire format, the
snapshot test fails loudly — bump `schema_version` and update the
fixture intentionally.

## Layout

```
gui_app/
├── gui_plan.md              # design spec
├── gui_ux_improvements.md   # UX redesign tracking
├── package.json
├── vite.config.ts
├── index.html
├── src/
│   ├── main.tsx
│   ├── App.tsx              # project bar + 3-step nav + history panel
│   ├── components/
│   │   ├── ProjectBar.tsx
│   │   ├── FeatureCountsImport.tsx
│   │   ├── ValidationPanel.tsx
│   │   ├── ProfileSummary.tsx
│   │   ├── RunHistoryPanel.tsx
│   │   ├── ResultsTable.tsx
│   │   ├── TauriRequiredBanner.tsx
│   │   └── plots/{Volcano,MAPlot,types}.tsx
│   ├── pages/
│   │   ├── DataPage.tsx     # step 1
│   │   ├── AnalyzePage.tsx  # step 2 (validate + run)
│   │   └── ResultsPage.tsx  # step 3
│   ├── lib/
│   │   ├── project.ts       # mirrors project.rs
│   │   ├── results.ts       # mirrors results.rs
│   │   ├── validation.ts    # mirrors validation.rs
│   │   ├── featurecounts.ts # mirrors featurecounts.rs
│   │   ├── analysis.ts      # mirrors flashdeg.rs run types
│   │   ├── requests.ts      # project -> backend request mapping
│   │   ├── project_context.tsx # always-on project + app settings
│   │   ├── app_settings.ts  # localStorage (binary path, threads)
│   │   ├── colors.ts
│   │   └── tauri.ts         # typed IPC wrappers
│   └── styles/global.css
└── src-tauri/
    ├── Cargo.toml
    ├── tauri.conf.json
    ├── build.rs
    ├── capabilities/default.json
    ├── src/
    │   ├── main.rs
    │   ├── lib.rs           # Tauri builder + invoke_handler
    │   ├── commands.rs      # IPC commands
    │   ├── project.rs       # project file schema (source of truth)
    │   ├── results.rs       # results CSV parser
    │   ├── validation.rs    # § 6 / § 7 checks
    │   ├── featurecounts.rs # § 5.2 import
    │   ├── flashdeg.rs      # § 11 spawn / cancel / profile
    │   └── paths.rs         # stub: § 13
    └── tests/
        ├── project_fixtures.rs
        └── fixtures/*.flashdeg
```
