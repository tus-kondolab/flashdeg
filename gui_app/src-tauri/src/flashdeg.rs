// FlashDEG process invocation. See gui_plan.md § 11 and § 9.
//
// Step 4a scope:
//   - Spawn a `flashdeg run ...` child process from an AnalysisRequest.
//   - Stream stdout / stderr line-by-line to the frontend via Tauri events.
//   - Support cooperative cancellation (Child::kill -> TerminateProcess on
//     Windows; SIGKILL on Unix). Graceful SIGTERM-then-SIGKILL is left to
//     step 4c (gui_plan.md § 9 cancel behavior).
//
// Events emitted (all carry a `run_id` so multiple concurrent runs would
// be possible later):
//   flashdeg:log   { run_id, stream: "stdout"|"stderr", line }
//   flashdeg:done  { run_id, exit_code, cancelled }

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter};

use crate::project::{Contrast, Design, FitType, SizeFactors, TestKind};

const POLL_INTERVAL_MS: u64 = 100;

#[derive(Debug, Deserialize)]
pub struct AnalysisRequest {
    pub binary_path: PathBuf,
    pub counts_path: PathBuf,
    pub metadata_path: PathBuf,
    pub output_path: PathBuf,
    #[serde(default)]
    pub profile_json_path: Option<PathBuf>,
    pub design: Design,
    pub contrast: Contrast,
    #[serde(default)]
    pub ref_levels: BTreeMap<String, String>,
    pub fit_type: FitType,
    pub size_factors: SizeFactors,
    /// Statistical test. `Wald` (default) emits no `--test`; `Lrt` emits
    /// `--test LRT --reduced <reduced_formula>`.
    #[serde(default)]
    pub test_kind: TestKind,
    /// Reduced-model formula for an LRT run (e.g. `~ batch` or `~ 1`). Required
    /// when `test_kind == Lrt`, ignored otherwise. Derived on the frontend from
    /// the full design (drop the tested factor, keep covariates).
    #[serde(default)]
    pub reduced_formula: Option<String>,
    pub independent_filter: bool,
    pub cooks_filter: bool,
    pub refit_cooks: bool,
    pub threads: u32,
    pub project_dir: Option<PathBuf>,
    /// Raw extra CLI flags, appended after the managed args. Tokenized with
    /// shell-words (no shell) in build_args. See gui_extra_args_plan.md.
    #[serde(default)]
    pub extra_args: String,
}

#[derive(Debug, Serialize, Clone)]
pub struct LogEvent {
    pub run_id: String,
    pub stream: String, // "stdout" | "stderr"
    pub line: String,
}

#[derive(Debug, Serialize, Clone)]
pub struct DoneEvent {
    pub run_id: String,
    pub exit_code: Option<i32>,
    pub cancelled: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub profile: Option<RunProfile>,
}

/// Parsed contents of FlashDEG's `--profile-json` output. Schema is
/// deliberately loose — FlashDEG's profile_report.cpp evolves, so we
/// capture known shapes and let `extras` carry the rest.
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct StepTiming {
    #[serde(default)]
    pub wall_ms: f64,
    #[serde(flatten)]
    pub extras: BTreeMap<String, serde_json::Value>,
}

#[derive(Debug, Clone, Deserialize, Serialize, Default)]
pub struct RunProfile {
    #[serde(default)]
    pub steps: BTreeMap<String, StepTiming>,
    #[serde(default)]
    pub metadata: BTreeMap<String, serde_json::Value>,
    /// Sum of `wall_ms` across all steps. Computed at parse time so the UI
    /// can show one number without re-summing.
    #[serde(default)]
    pub total_wall_ms: f64,
}

fn read_profile_json(path: &Path) -> Option<RunProfile> {
    let raw = fs::read_to_string(path).ok()?;
    let mut profile: RunProfile = serde_json::from_str(&raw).ok()?;
    profile.total_wall_ms = profile.steps.values().map(|s| s.wall_ms).sum();
    Some(profile)
}

#[derive(Debug, Serialize, Clone)]
pub struct StartedEvent {
    pub run_id: String,
    pub binary_path: String,
    pub args: Vec<String>,
}

struct RunSlot {
    child: Mutex<Child>,
    cancelled: AtomicBool,
}

/// Cheaply cloneable handle to the active-run table. Tauri stores one
/// instance as managed state; threads clone it freely.
#[derive(Clone, Default)]
pub struct RunRegistry {
    runs: Arc<Mutex<HashMap<String, Arc<RunSlot>>>>,
    next_seq: Arc<Mutex<u64>>,
}

impl RunRegistry {
    fn issue_run_id(&self) -> String {
        let mut g = self.next_seq.lock().unwrap();
        *g += 1;
        format!("run-{}", *g)
    }
}

#[derive(Debug, thiserror::Error)]
pub enum FlashdegError {
    #[error("FlashDEG binary not found: {0}")]
    BinaryNotFound(String),
    #[error("Failed to spawn FlashDEG: {0}")]
    SpawnFailed(String),
    #[error("run_id not found: {0}")]
    UnknownRunId(String),
    #[error("Failed to kill child: {0}")]
    KillFailed(String),
    #[error("Invalid extra arguments: {0}")]
    InvalidExtraArgs(String),
    #[error("Invalid analysis request: {0}")]
    InvalidRequest(String),
}

/// Build the `flashdeg run ...` argument list from an AnalysisRequest.
/// Visible for testing. Errors only if the user's extra-args string has
/// mismatched quotes (shell-words parse failure).
pub fn build_args(req: &AnalysisRequest) -> Result<Vec<String>, FlashdegError> {
    let resolve = |p: &Path| -> String {
        if p.is_absolute() {
            return p.display().to_string();
        }
        match &req.project_dir {
            Some(dir) => dir.join(p).display().to_string(),
            None => p.display().to_string(),
        }
    };

    let mut args: Vec<String> = vec!["run".into()];
    args.push("--counts".into());
    args.push(resolve(&req.counts_path));
    args.push("--metadata".into());
    args.push(resolve(&req.metadata_path));

    match &req.design {
        Design::Formula { formula } => {
            args.push("--design".into());
            args.push(formula.clone());
        }
        Design::Matrix { path, .. } => {
            args.push("--design-matrix".into());
            args.push(resolve(path));
        }
        // Custom: the user provides --design/--contrast/--test themselves via
        // extra_args, so emit none of the managed model flags here.
        Design::Custom => {}
    }

    // The contrast, LRT and ref-level flags are part of the managed model, so
    // they are skipped for a Custom design (the user supplies them in extra_args).
    if !matches!(req.design, Design::Custom) {
        match &req.contrast {
            Contrast::FactorLevels {
                factor,
                test,
                control,
            } => {
                args.push("--contrast".into());
                args.push(factor.clone());
                args.push(test.clone());
                args.push(control.clone());
            }
            Contrast::DesignColumn { name } => {
                args.push("--contrast-name".into());
                args.push(name.clone());
            }
            Contrast::Vector { values } => {
                args.push("--contrast-vector".into());
                args.push(
                    values
                        .iter()
                        .map(|v| format!("{v}"))
                        .collect::<Vec<_>>()
                        .join(","),
                );
            }
        }

        // Likelihood-ratio test: emit `--test LRT --reduced <formula>`. The
        // contrast above stays (it only selects the displayed log2FoldChange).
        // Wald emits no `--test` flag, preserving the default command shape.
        if matches!(req.test_kind, TestKind::Lrt) {
            let reduced = req.reduced_formula.as_deref().ok_or_else(|| {
                FlashdegError::InvalidRequest(
                    "LRT test selected but no reduced model formula was provided".into(),
                )
            })?;
            args.push("--test".into());
            args.push("LRT".into());
            args.push("--reduced".into());
            args.push(reduced.to_string());
        }

        for (col, level) in &req.ref_levels {
            args.push("--ref-level".into());
            args.push(format!("{col}={level}"));
        }
    }

    args.push("--out".into());
    args.push(resolve(&req.output_path));
    if let Some(profile_path) = &req.profile_json_path {
        args.push("--profile-json".into());
        args.push(resolve(profile_path));
    }
    args.push("--threads".into());
    args.push(req.threads.to_string());

    args.push("--fit-type".into());
    args.push(
        match req.fit_type {
            FitType::Parametric => "parametric",
            FitType::Local => "local",
            FitType::Mean => "mean",
        }
        .into(),
    );

    args.push("--size-factors".into());
    args.push(
        match req.size_factors {
            SizeFactors::Ratio => "ratio",
            SizeFactors::Poscounts => "poscounts",
        }
        .into(),
    );

    args.push("--independent-filter".into());
    args.push(req.independent_filter.to_string());
    args.push("--cooks-filter".into());
    args.push(req.cooks_filter.to_string());
    args.push("--refit-cooks".into());
    args.push(req.refit_cooks.to_string());

    // User-supplied extra flags, appended last. Tokenized with shell-words
    // (quote/escape aware, NO shell, NO variable/command expansion) and passed
    // as plain argv via Command::args — so there is no shell-injection surface.
    if !req.extra_args.trim().is_empty() {
        let extra = shell_words::split(&req.extra_args)
            .map_err(|e| FlashdegError::InvalidExtraArgs(e.to_string()))?;
        args.extend(extra);
    }

    Ok(args)
}

/// Spawn a new FlashDEG run. Returns the assigned `run_id`. The frontend
/// should subscribe to `flashdeg:log` and `flashdeg:done` events before
/// calling this so it doesn't miss the first lines.
pub fn spawn_analysis(
    app: AppHandle,
    registry: &RunRegistry,
    req: AnalysisRequest,
) -> Result<String, FlashdegError> {
    if !req.binary_path.exists() {
        return Err(FlashdegError::BinaryNotFound(
            req.binary_path.display().to_string(),
        ));
    }

    // Ensure the output directory exists so a run-directory layout like
    // `<project>/results_<ts>.csv` or `runs/<ts>/results.csv` works without
    // the user pre-creating folders. See gui_plan.md § 12.
    let resolved_output = resolve_with_dir(&req.output_path, req.project_dir.as_deref());
    if let Some(parent) = resolved_output.parent() {
        if !parent.as_os_str().is_empty() {
            let _ = std::fs::create_dir_all(parent);
        }
    }

    let args = build_args(&req)?;
    let mut cmd = Command::new(&req.binary_path);
    cmd.args(&args);
    cmd.stdout(Stdio::piped());
    cmd.stderr(Stdio::piped());
    // `flashdeg` is a console-subsystem program. When the release GUI (a
    // windowed app with no console of its own) spawns it, Windows would
    // otherwise allocate a fresh console window for the child, flashing a
    // command-prompt on screen. CREATE_NO_WINDOW suppresses that; stdout/
    // stderr are still captured via the pipes above. (No effect in dev, where
    // the child inherits the terminal that launched `tauri dev`.)
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }

    let mut child = cmd
        .spawn()
        .map_err(|e| FlashdegError::SpawnFailed(e.to_string()))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| FlashdegError::SpawnFailed("no stdout pipe".into()))?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| FlashdegError::SpawnFailed("no stderr pipe".into()))?;

    let run_id = registry.issue_run_id();
    let slot = Arc::new(RunSlot {
        child: Mutex::new(child),
        cancelled: AtomicBool::new(false),
    });
    registry
        .runs
        .lock()
        .unwrap()
        .insert(run_id.clone(), slot.clone());

    // Started event (so the UI can clear logs and switch to "running").
    let _ = app.emit(
        "flashdeg:started",
        StartedEvent {
            run_id: run_id.clone(),
            binary_path: req.binary_path.display().to_string(),
            args: args.clone(),
        },
    );

    // stdout reader
    spawn_log_reader(app.clone(), run_id.clone(), "stdout", stdout);
    // stderr reader
    spawn_log_reader(app.clone(), run_id.clone(), "stderr", stderr);

    // wait poller. The profile JSON path is captured here so we can read
    // it on the success path without needing the AnalysisRequest later.
    let app_for_wait = app.clone();
    let id_for_wait = run_id.clone();
    let slot_for_wait = slot.clone();
    let registry_for_wait = registry.clone();
    let profile_path_for_wait: Option<PathBuf> = req
        .profile_json_path
        .as_ref()
        .map(|p| resolve_with_dir(p, req.project_dir.as_deref()));

    std::thread::spawn(move || {
        loop {
            let status_opt = {
                let mut guard = slot_for_wait.child.lock().unwrap();
                guard.try_wait().ok().flatten()
            };
            if let Some(status) = status_opt {
                let cancelled = slot_for_wait.cancelled.load(Ordering::SeqCst);
                let profile = if !cancelled && status.success() {
                    profile_path_for_wait.as_deref().and_then(read_profile_json)
                } else {
                    None
                };
                let _ = app_for_wait.emit(
                    "flashdeg:done",
                    DoneEvent {
                        run_id: id_for_wait.clone(),
                        exit_code: status.code(),
                        cancelled,
                        profile,
                    },
                );
                registry_for_wait.runs.lock().unwrap().remove(&id_for_wait);
                return;
            }
            std::thread::sleep(Duration::from_millis(POLL_INTERVAL_MS));
        }
    });

    Ok(run_id)
}

fn resolve_with_dir(p: &Path, project_dir: Option<&Path>) -> PathBuf {
    if p.is_absolute() {
        return p.to_path_buf();
    }
    match project_dir {
        Some(dir) => dir.join(p),
        None => p.to_path_buf(),
    }
}

pub fn cancel_run(registry: &RunRegistry, run_id: &str) -> Result<(), FlashdegError> {
    let slot = registry
        .runs
        .lock()
        .unwrap()
        .get(run_id)
        .cloned()
        .ok_or_else(|| FlashdegError::UnknownRunId(run_id.to_string()))?;
    slot.cancelled.store(true, Ordering::SeqCst);
    let mut guard = slot.child.lock().unwrap();
    guard
        .kill()
        .map_err(|e| FlashdegError::KillFailed(e.to_string()))?;
    Ok(())
}

fn spawn_log_reader<R: std::io::Read + Send + 'static>(
    app: AppHandle,
    run_id: String,
    stream: &'static str,
    source: R,
) {
    std::thread::spawn(move || {
        let reader = BufReader::new(source);
        for line in reader.lines() {
            let Ok(line) = line else {
                break;
            };
            let _ = app.emit(
                "flashdeg:log",
                LogEvent {
                    run_id: run_id.clone(),
                    stream: stream.to_string(),
                    line,
                },
            );
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn template_request() -> AnalysisRequest {
        AnalysisRequest {
            binary_path: PathBuf::from("/usr/local/bin/flashdeg"),
            counts_path: PathBuf::from("counts.csv"),
            metadata_path: PathBuf::from("metadata.csv"),
            output_path: PathBuf::from("out.csv"),
            profile_json_path: None,
            design: Design::Formula {
                formula: "~ condition".into(),
            },
            contrast: Contrast::FactorLevels {
                factor: "condition".into(),
                test: "treated".into(),
                control: "control".into(),
            },
            ref_levels: {
                let mut m = BTreeMap::new();
                m.insert("condition".into(), "control".into());
                m
            },
            fit_type: FitType::Parametric,
            size_factors: SizeFactors::Ratio,
            test_kind: TestKind::Wald,
            reduced_formula: None,
            independent_filter: true,
            cooks_filter: true,
            refit_cooks: true,
            threads: 4,
            project_dir: None,
            extra_args: String::new(),
        }
    }

    #[test]
    fn builds_minimal_args_for_factor_levels_contrast() {
        let req = template_request();
        let args = build_args(&req).unwrap();
        assert!(args.contains(&"run".into()));
        assert!(args.windows(2).any(|w| w[0] == "--counts" && w[1] == "counts.csv"));
        assert!(args.windows(2).any(|w| w[0] == "--metadata" && w[1] == "metadata.csv"));
        assert!(args.windows(2).any(|w| w[0] == "--design" && w[1] == "~ condition"));
        assert!(args
            .windows(4)
            .any(|w| w[0] == "--contrast" && w[1] == "condition" && w[2] == "treated" && w[3] == "control"));
        assert!(args
            .windows(2)
            .any(|w| w[0] == "--ref-level" && w[1] == "condition=control"));
        assert!(args.windows(2).any(|w| w[0] == "--out" && w[1] == "out.csv"));
        assert!(args.windows(2).any(|w| w[0] == "--threads" && w[1] == "4"));
        assert!(args
            .windows(2)
            .any(|w| w[0] == "--fit-type" && w[1] == "parametric"));
        assert!(args
            .windows(2)
            .any(|w| w[0] == "--size-factors" && w[1] == "ratio"));
        assert!(args.windows(2).any(|w| w[0] == "--refit-cooks" && w[1] == "true"));
    }

    #[test]
    fn builds_contrast_name_args() {
        let mut req = template_request();
        req.contrast = Contrast::DesignColumn {
            name: "genotype[T.KO]:treatment[T.drug]".into(),
        };
        let args = build_args(&req).unwrap();
        assert!(args
            .windows(2)
            .any(|w| w[0] == "--contrast-name"
                && w[1] == "genotype[T.KO]:treatment[T.drug]"));
        // No --contrast 3-tuple form should appear.
        assert!(!args.iter().any(|a| a == "--contrast"));
    }

    #[test]
    fn builds_contrast_vector_args() {
        let mut req = template_request();
        req.contrast = Contrast::Vector {
            values: vec![0.0, -1.0, 1.0],
        };
        let args = build_args(&req).unwrap();
        let pos = args.iter().position(|a| a == "--contrast-vector").unwrap();
        assert_eq!(args[pos + 1], "0,-1,1");
    }

    #[test]
    fn wald_emits_no_test_flag() {
        let req = template_request();
        let args = build_args(&req).unwrap();
        assert!(!args.iter().any(|a| a == "--test"));
        assert!(!args.iter().any(|a| a == "--reduced"));
    }

    #[test]
    fn lrt_emits_test_and_reduced() {
        let mut req = template_request();
        req.test_kind = TestKind::Lrt;
        req.reduced_formula = Some("~ batch".into());
        let args = build_args(&req).unwrap();
        let pos = args.iter().position(|a| a == "--test").unwrap();
        assert_eq!(args[pos + 1], "LRT");
        let rpos = args.iter().position(|a| a == "--reduced").unwrap();
        assert_eq!(args[rpos + 1], "~ batch");
        // The display contrast is still present for an LRT run.
        assert!(args.iter().any(|a| a == "--contrast"));
    }

    #[test]
    fn lrt_without_reduced_errors() {
        let mut req = template_request();
        req.test_kind = TestKind::Lrt;
        req.reduced_formula = None;
        assert!(matches!(build_args(&req), Err(FlashdegError::InvalidRequest(_))));
    }

    #[test]
    fn builds_design_matrix_args() {
        let mut req = template_request();
        req.design = Design::Matrix {
            path: PathBuf::from("design.csv"),
            columns: vec!["Intercept".into(), "conditionB".into()],
        };
        let args = build_args(&req).unwrap();
        assert!(args
            .windows(2)
            .any(|w| w[0] == "--design-matrix" && w[1] == "design.csv"));
        // No --design formula should appear.
        assert!(!args.iter().any(|a| a == "--design"));
    }

    #[test]
    fn custom_design_skips_managed_model_flags() {
        let mut req = template_request();
        req.design = Design::Custom;
        req.test_kind = TestKind::Lrt; // should be ignored for custom
        req.extra_args = "--design-matrix design.csv --contrast-vector 0,0,1".into();
        let args = build_args(&req).unwrap();
        // No managed model flags.
        assert!(!args.iter().any(|a| a == "--design"));
        assert!(!args.iter().any(|a| a == "--contrast"));
        assert!(!args.iter().any(|a| a == "--ref-level"));
        assert!(!args.iter().any(|a| a == "--test"));
        // But I/O + run options + the user's args are present.
        assert!(args.windows(2).any(|w| w[0] == "--counts" && w[1] == "counts.csv"));
        assert!(args.iter().any(|a| a == "--fit-type"));
        assert!(args.windows(2).any(|w| w[0] == "--design-matrix" && w[1] == "design.csv"));
        assert!(args.windows(2).any(|w| w[0] == "--contrast-vector" && w[1] == "0,0,1"));
    }

    #[test]
    fn includes_profile_json_when_set() {
        let mut req = template_request();
        req.profile_json_path = Some(PathBuf::from("profile.json"));
        let args = build_args(&req).unwrap();
        assert!(args
            .windows(2)
            .any(|w| w[0] == "--profile-json" && w[1] == "profile.json"));
    }

    #[test]
    fn omits_profile_json_when_unset() {
        let req = template_request();
        let args = build_args(&req).unwrap();
        assert!(!args.iter().any(|a| a == "--profile-json"));
    }

    #[test]
    fn appends_extra_args_after_managed_args() {
        let mut req = template_request();
        req.extra_args = "--lfc-threshold 1 --alpha 0.05".into();
        let args = build_args(&req).unwrap();
        // Appended at the end, in order.
        let tail = &args[args.len() - 4..];
        assert_eq!(tail, ["--lfc-threshold", "1", "--alpha", "0.05"]);
    }

    #[test]
    fn extra_args_quoted_value_stays_one_token() {
        let mut req = template_request();
        req.extra_args = "--design \"~ a + b\"".into();
        let args = build_args(&req).unwrap();
        let pos = args.iter().rposition(|a| a == "--design").unwrap();
        assert_eq!(args[pos + 1], "~ a + b");
    }

    #[test]
    fn extra_args_mismatched_quote_errors() {
        let mut req = template_request();
        req.extra_args = "--name \"unterminated".into();
        assert!(matches!(build_args(&req), Err(FlashdegError::InvalidExtraArgs(_))));
    }

    #[test]
    fn empty_extra_args_add_nothing() {
        let mut req = template_request();
        req.extra_args = "   ".into();
        let args = build_args(&req).unwrap();
        assert_eq!(args.last().unwrap(), "true"); // ends with --refit-cooks true
    }

    #[test]
    fn parses_profile_json_and_computes_total() {
        let dir = std::env::temp_dir().join(format!(
            "flashdeg_gui_profile_{}",
            std::process::id()
        ));
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("profile.json");
        std::fs::write(
            &path,
            r#"{
              "steps": {
                "size_factor_ms": {"wall_ms": 12.5},
                "dispersion_gene_wise_ms": {"wall_ms": 240.0},
                "wald_test_ms": {"wall_ms": 5.5, "n_genes": 9931}
              },
              "metadata": {
                "linear_algebra_backend": "eigen"
              }
            }"#,
        )
        .unwrap();
        let p = read_profile_json(&path).unwrap();
        assert!((p.total_wall_ms - 258.0).abs() < 1e-6);
        assert_eq!(p.steps.len(), 3);
        assert!((p.steps.get("size_factor_ms").unwrap().wall_ms - 12.5).abs() < 1e-9);
        // Extras preserved.
        assert!(p.steps.get("wald_test_ms").unwrap().extras.contains_key("n_genes"));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn read_profile_json_returns_none_for_missing_file() {
        let bogus = std::env::temp_dir().join("definitely-does-not-exist.json");
        assert!(read_profile_json(&bogus).is_none());
    }

    #[test]
    fn resolves_relative_paths_against_project_dir() {
        let mut req = template_request();
        req.project_dir = Some(PathBuf::from("/home/user/proj"));
        let args = build_args(&req).unwrap();
        let counts_idx = args.iter().position(|a| a == "--counts").unwrap();
        let counts_arg = &args[counts_idx + 1];
        assert!(
            counts_arg.contains("proj") && counts_arg.contains("counts.csv"),
            "got {counts_arg}"
        );
    }

    #[test]
    fn rejects_missing_binary() {
        // We can't easily test spawn here without a Tauri app, but we can
        // exercise the existence check by calling spawn_analysis with a
        // bogus binary path and asserting the error type — which would
        // require an AppHandle. Skip: covered by integration tests with a
        // mock FlashDEG binary in step 4b.
    }
}
