// Tauri IPC commands. Frontend invokes these by name; signatures must match
// the typed wrappers in src/lib/tauri.ts.

use std::path::PathBuf;

use tauri::{AppHandle, State};

use crate::featurecounts::{
    self, FeatureCountsInspection, MergeReport, MergeRequest, WriteMetadataReport,
    WriteMetadataRequest,
};
use crate::flashdeg::{self, AnalysisRequest, RunRegistry};
use crate::project::{self, Project};
use crate::results::{self, ResultsTable};
use crate::paths;
use crate::validation::{self, MetadataSummary, ValidationRequest, ValidationResult};

#[tauri::command]
pub fn default_project() -> Project {
    Project::default_template()
}

#[tauri::command]
pub fn load_project(path: String) -> Result<Project, String> {
    project::load(&PathBuf::from(path)).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn save_project(path: String, project: Project) -> Result<(), String> {
    project::save(&PathBuf::from(path), &project).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn load_results_csv(path: String) -> Result<ResultsTable, String> {
    results::load_csv(&PathBuf::from(path)).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn validate_inputs(request: ValidationRequest) -> ValidationResult {
    validation::validate(&request)
}

#[tauri::command]
pub fn inspect_metadata(path: String) -> Result<MetadataSummary, String> {
    validation::metadata_summary(&PathBuf::from(path))
}

#[tauri::command]
pub fn locate_flashdeg() -> Option<String> {
    paths::locate_flashdeg().map(|p| p.display().to_string())
}

/// Launch a brand-new, independent instance of the GUI (File ▸ New Window). A
/// separate process gets its own project, run registry, event bus, and menu —
/// so two analyses never cross-talk. The new instance starts with a fresh
/// untitled project; app-level settings (binary path, gene maps) are shared
/// via localStorage. The current window is untouched.
#[tauri::command]
pub fn launch_new_instance() -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    // macOS: when running from a .app bundle, `open -n -a <App>` spawns a clean
    // new instance with proper Launch Services / Dock handling. exe is
    // `<App>.app/Contents/MacOS/<bin>`, so the bundle is three parents up.
    #[cfg(target_os = "macos")]
    {
        if let Some(app) = exe.parent().and_then(|p| p.parent()).and_then(|p| p.parent()) {
            if app.extension().and_then(|e| e.to_str()) == Some("app") {
                std::process::Command::new("open")
                    .arg("-n")
                    .arg(app)
                    .spawn()
                    .map_err(|e| format!("failed to launch new instance: {e}"))?;
                return Ok(());
            }
        }
    }
    std::process::Command::new(&exe)
        .spawn()
        .map_err(|e| format!("failed to launch new instance: {e}"))?;
    Ok(())
}

/// Read the external gene-ID → symbol map (gzip bytes) for a species from the
/// `gene_maps/` folder beside the executable. The frontend decompresses and
/// parses it. Returns the raw `.tsv.gz` bytes as an ArrayBuffer to JS; errors
/// when the species is unknown or the file is absent (caller treats that as
/// "no symbols", showing raw IDs).
#[tauri::command]
pub fn read_gene_map(species: String) -> Result<tauri::ipc::Response, String> {
    let path = paths::locate_gene_map(&species)
        .ok_or_else(|| format!("no gene map file found for species '{species}'"))?;
    let bytes = std::fs::read(&path)
        .map_err(|e| format!("failed to read {}: {e}", path.display()))?;
    Ok(tauri::ipc::Response::new(bytes))
}

/// Read a user-supplied gene-ID -> symbol map from an arbitrary path (the
/// "Custom" gene map in Preferences). The user picks the file via an open
/// dialog, so reading the chosen path is expected. Plain `.tsv` or gzip
/// `.tsv.gz` are both fine; the frontend decompresses gzip. Returns the raw
/// bytes as an ArrayBuffer to JS.
#[tauri::command]
pub fn read_gene_map_file(path: String) -> Result<tauri::ipc::Response, String> {
    let p = PathBuf::from(&path);
    if !p.is_file() {
        return Err(format!("gene map file not found: {path}"));
    }
    let bytes = std::fs::read(&p).map_err(|e| format!("failed to read {path}: {e}"))?;
    Ok(tauri::ipc::Response::new(bytes))
}

/// Write raw bytes to a path, creating parent directories. Used to save
/// exported plot images next to the results they came from.
#[tauri::command]
pub fn save_plot(path: String, bytes: Vec<u8>) -> Result<(), String> {
    let p = PathBuf::from(path);
    if let Some(parent) = p.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
    }
    std::fs::write(&p, &bytes).map_err(|e| e.to_string())
}

/// Put an SVG on the clipboard as a vector graphic so apps like PowerPoint /
/// Word / Impress paste it as vector (the browser/WebView clipboard API can't
/// write this format). Cross-platform via clipboard-rs; the format name differs
/// per OS (macOS uses the SVG UTI, Windows/Linux use the MIME type).
#[tauri::command]
pub fn copy_svg(svg: String) -> Result<(), String> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    #[cfg(target_os = "macos")]
    let format = "public.svg-image";
    #[cfg(not(target_os = "macos"))]
    let format = "image/svg+xml";
    let ctx = ClipboardContext::new().map_err(|e| e.to_string())?;
    ctx.set_buffer(format, svg.into_bytes()).map_err(|e| e.to_string())?;
    Ok(())
}

/// Tokenize the user's "extra arguments" string for live preview / validation,
/// using the SAME shell-words splitter as the actual run (quote/escape aware,
/// no shell). Returns the resulting argv tokens, or an error string on
/// mismatched quotes. Display-only; the run re-splits the raw string itself.
#[tauri::command]
pub fn validate_extra_args(text: String) -> Result<Vec<String>, String> {
    shell_words::split(&text).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn run_analysis(
    app: AppHandle,
    registry: State<'_, RunRegistry>,
    request: AnalysisRequest,
) -> Result<String, String> {
    flashdeg::spawn_analysis(app, registry.inner(), request).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn cancel_analysis(
    registry: State<'_, RunRegistry>,
    run_id: String,
) -> Result<(), String> {
    flashdeg::cancel_run(registry.inner(), &run_id).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn inspect_featurecounts(path: String) -> Result<FeatureCountsInspection, String> {
    featurecounts::inspect(&PathBuf::from(path)).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn merge_featurecounts(request: MergeRequest) -> Result<MergeReport, String> {
    featurecounts::merge_and_write(&request).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn write_metadata_csv(
    request: WriteMetadataRequest,
) -> Result<WriteMetadataReport, String> {
    featurecounts::write_metadata(&request).map_err(|e| e.to_string())
}
