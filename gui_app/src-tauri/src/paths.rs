// Locate the `flashdeg` binary. See gui_plan.md § 13.
//
// Resolution order (highest priority first):
//   1. Co-located: a `flashdeg` sitting in the SAME directory as the GUI
//      executable. A self-contained folder / .app ships its own engine, and it
//      should win over anything else on the machine.
//   2. PATH: e.g. an engine installed system-wide via the .msi.
//   3. The folder "above" the GUI — on macOS the folder that CONTAINS the .app
//      (what the user sees in Finder); otherwise the literal parent directory.
// A user-set override is handled in the frontend.

use std::path::PathBuf;

pub fn flashdeg_binary_name() -> &'static str {
    if cfg!(windows) {
        "flashdeg.exe"
    } else {
        "flashdeg"
    }
}

/// Directory holding the running GUI executable.
fn exe_dir() -> Option<PathBuf> {
    Some(std::env::current_exe().ok()?.parent()?.to_path_buf())
}

/// (1) Right next to the GUI executable. On Windows/Linux this is the install
/// directory; on macOS it is `Foo.app/Contents/MacOS` (the bundled-sidecar slot).
pub fn locate_next_to_exe() -> Option<PathBuf> {
    let candidate = exe_dir()?.join(flashdeg_binary_name());
    if candidate.is_file() {
        Some(candidate)
    } else {
        None
    }
}

/// (2) Search the PATH environment variable for the flashdeg executable.
pub fn locate_on_path() -> Option<PathBuf> {
    let name = flashdeg_binary_name();
    let path_var = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path_var) {
        if dir.as_os_str().is_empty() {
            continue;
        }
        let candidate = dir.join(name);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

/// (3) The folder "above" the GUI. On macOS, if the GUI runs from
/// `Foo.app/Contents/MacOS`, this is the folder CONTAINING `Foo.app` (what the
/// user sees in Finder); otherwise it is the literal parent of the executable's
/// directory.
pub fn locate_in_parent() -> Option<PathBuf> {
    let dir = exe_dir()?;
    let name = flashdeg_binary_name();

    // macOS .app: prefer the folder containing the bundle. Guarded by the
    // `.app` suffix, so it is a no-op on other platforms.
    if let Some(app) = dir.parent().and_then(|contents| contents.parent()) {
        if app.extension().and_then(|e| e.to_str()) == Some("app") {
            if let Some(container) = app.parent() {
                let candidate = container.join(name);
                if candidate.is_file() {
                    return Some(candidate);
                }
            }
        }
    }

    let candidate = dir.parent()?.join(name);
    if candidate.is_file() {
        Some(candidate)
    } else {
        None
    }
}

/// Co-located → PATH → parent / .app-container.
pub fn locate_flashdeg() -> Option<PathBuf> {
    locate_next_to_exe()
        .or_else(locate_on_path)
        .or_else(locate_in_parent)
}

/// External, user-editable gene-ID → symbol map. The maps are NOT embedded in
/// the executable; they ship as `.tsv.gz` files in a `gene_maps/` folder next
/// to the GUI executable so they can be replaced by hand. Returns None for an
/// unknown species or a missing file — the symbol toggle then degrades to
/// showing raw gene IDs.
pub fn locate_gene_map(species: &str) -> Option<PathBuf> {
    let file = match species {
        "human" => "gene_symbols_human.tsv.gz",
        "fly" => "gene_symbols_fly.tsv.gz",
        _ => return None,
    };
    // (1) Distributed layout: `gene_maps/` beside the executable.
    if let Some(dir) = exe_dir() {
        let candidate = dir.join("gene_maps").join(file);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    // (2) Dev convenience (debug builds only): the repo's `gui_app/gene_maps`.
    // CARGO_MANIFEST_DIR is `gui_app/src-tauri` at build time, so `..` is
    // `gui_app`. This lets `npm run tauri:dev` resolve the maps without copying
    // them next to the throwaway debug binary.
    #[cfg(debug_assertions)]
    {
        let candidate = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("..")
            .join("gene_maps")
            .join(file);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}
