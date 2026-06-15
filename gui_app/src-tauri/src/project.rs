// Project file schema. Source of truth for the .flashdeg JSON wire format.
// Mirrored in TypeScript at src/lib/project.ts.
//
// Discriminated unions use `#[serde(tag = "kind", rename_all = "snake_case")]`
// to produce JSON of the form {"kind": "factor_levels", ...}, matching the
// shapes documented in gui_plan.md § 4.1.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

pub const CURRENT_SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Contrast {
    FactorLevels {
        factor: String,
        test: String,
        control: String,
    },
    DesignColumn {
        name: String,
    },
    Vector {
        values: Vec<f64>,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Design {
    Formula {
        formula: String,
    },
    Matrix {
        path: PathBuf,
        columns: Vec<String>,
    },
    /// Fully custom: the user supplies the entire model (design/contrast/test)
    /// via raw CLI args (Options::extra_args). build_args emits none of the
    /// managed `--design`/`--contrast`/`--ref-level`/`--test` flags for it.
    Custom,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Inputs {
    pub counts: PathBuf,
    pub metadata: PathBuf,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum FitType {
    Parametric,
    Local,
    Mean,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum SizeFactors {
    Ratio,
    Poscounts,
}

/// Which statistical test the run uses. `Wald` is the standard two-group DEG
/// path; `Lrt` is the likelihood-ratio test of the full design against a nested
/// reduced model (the reduced model is derived at run time, not stored here).
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
#[serde(rename_all = "lowercase")]
pub enum TestKind {
    #[default]
    Wald,
    Lrt,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Options {
    pub fit_type: FitType,
    pub size_factors: SizeFactors,
    pub independent_filter: bool,
    pub cooks_filter: bool,
    pub refit_cooks: bool,
    /// Statistical test for the run. Default `Wald` (back-compat with projects
    /// written before this field existed).
    #[serde(default)]
    pub test_kind: TestKind,
    /// Raw, user-typed extra CLI flags appended to the flashdeg command. Split
    /// with shell-words at run time (no shell). Default empty (back-compat with
    /// projects written before this field existed). See gui_extra_args_plan.md.
    #[serde(default)]
    pub extra_args: String,
}

/// Snapshot of the analysis configuration a run was launched with, so the run
/// history records "what settings produced this result" and can restore them.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RunParams {
    pub inputs: Inputs,
    pub design: Design,
    pub contrast: Contrast,
    #[serde(default)]
    pub ref_levels: BTreeMap<String, String>,
    pub options: Options,
    pub threads: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RunRecord {
    pub timestamp: String,
    pub results_path: PathBuf,
    pub flashdeg_version: String,
    pub git_revision: String,
    /// Parameters used for this run. Optional for back-compat with projects
    /// written before run-parameter snapshots existed.
    #[serde(default)]
    pub params: Option<RunParams>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Project {
    pub schema_version: u32,
    pub inputs: Inputs,
    pub design: Design,
    pub contrast: Contrast,
    pub ref_levels: BTreeMap<String, String>,
    pub options: Options,
    pub runs: Vec<RunRecord>,
}

impl Project {
    pub fn default_template() -> Self {
        let mut ref_levels = BTreeMap::new();
        ref_levels.insert("condition".to_string(), "control".to_string());
        Self {
            schema_version: CURRENT_SCHEMA_VERSION,
            inputs: Inputs {
                counts: PathBuf::new(),
                metadata: PathBuf::new(),
            },
            design: Design::Formula {
                formula: "~ condition".to_string(),
            },
            contrast: Contrast::FactorLevels {
                factor: "condition".to_string(),
                test: "treated".to_string(),
                control: "control".to_string(),
            },
            ref_levels,
            options: Options {
                fit_type: FitType::Parametric,
                size_factors: SizeFactors::Ratio,
                independent_filter: true,
                cooks_filter: true,
                refit_cooks: true,
                test_kind: TestKind::Wald,
                extra_args: String::new(),
            },
            runs: Vec::new(),
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum ProjectError {
    #[error("I/O error reading {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("invalid project JSON in {path}: {source}")]
    Parse {
        path: PathBuf,
        #[source]
        source: serde_json::Error,
    },
    #[error("unsupported schema_version {found} (this build supports up to {max})")]
    UnsupportedVersion { found: u32, max: u32 },
}

pub fn load(path: &Path) -> Result<Project, ProjectError> {
    let raw = fs::read_to_string(path).map_err(|source| ProjectError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    let project: Project =
        serde_json::from_str(&raw).map_err(|source| ProjectError::Parse {
            path: path.to_path_buf(),
            source,
        })?;
    if project.schema_version > CURRENT_SCHEMA_VERSION {
        return Err(ProjectError::UnsupportedVersion {
            found: project.schema_version,
            max: CURRENT_SCHEMA_VERSION,
        });
    }
    Ok(project)
}

pub fn save(path: &Path, project: &Project) -> Result<(), ProjectError> {
    let json = serde_json::to_string_pretty(project).map_err(|source| ProjectError::Parse {
        path: path.to_path_buf(),
        source,
    })?;
    fs::write(path, json).map_err(|source| ProjectError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(value: &Project) -> Project {
        let json = serde_json::to_string_pretty(value).expect("serialize");
        serde_json::from_str(&json).expect("deserialize")
    }

    #[test]
    fn default_template_round_trips() {
        let p = Project::default_template();
        assert_eq!(p, roundtrip(&p));
    }

    #[test]
    fn contrast_factor_levels_round_trips() {
        let json = r#"{"kind":"factor_levels","factor":"condition","test":"treated","control":"control"}"#;
        let c: Contrast = serde_json::from_str(json).unwrap();
        match &c {
            Contrast::FactorLevels { factor, test, control } => {
                assert_eq!(factor, "condition");
                assert_eq!(test, "treated");
                assert_eq!(control, "control");
            }
            _ => panic!("wrong variant"),
        }
        let back = serde_json::to_string(&c).unwrap();
        assert!(back.contains(r#""kind":"factor_levels""#));
    }

    #[test]
    fn contrast_design_column_round_trips() {
        let json = r#"{"kind":"design_column","name":"genotype[T.KO]:treatment[T.drug]"}"#;
        let c: Contrast = serde_json::from_str(json).unwrap();
        assert_eq!(serde_json::to_value(&c).unwrap()["kind"], "design_column");
    }

    #[test]
    fn contrast_vector_round_trips() {
        let json = r#"{"kind":"vector","values":[0,-1,1]}"#;
        let c: Contrast = serde_json::from_str(json).unwrap();
        if let Contrast::Vector { values } = &c {
            assert_eq!(values, &vec![0.0, -1.0, 1.0]);
        } else {
            panic!("wrong variant");
        }
    }

    #[test]
    fn design_matrix_round_trips() {
        let json = r#"{"kind":"matrix","path":"design_matrix.csv","columns":["Intercept","conditionB"]}"#;
        let d: Design = serde_json::from_str(json).unwrap();
        if let Design::Matrix { columns, .. } = &d {
            assert_eq!(columns, &vec!["Intercept".to_string(), "conditionB".to_string()]);
        } else {
            panic!("wrong variant");
        }
    }

    #[test]
    fn reject_future_schema_version() {
        let dir = tempdir_stub();
        let path = dir.join("future.flashdeg");
        let mut p = Project::default_template();
        p.schema_version = CURRENT_SCHEMA_VERSION + 1;
        let json = serde_json::to_string_pretty(&p).unwrap();
        std::fs::write(&path, json).unwrap();
        let err = load(&path).unwrap_err();
        match err {
            ProjectError::UnsupportedVersion { found, max } => {
                assert_eq!(found, CURRENT_SCHEMA_VERSION + 1);
                assert_eq!(max, CURRENT_SCHEMA_VERSION);
            }
            other => panic!("wrong error: {other:?}"),
        }
        let _ = std::fs::remove_file(&path);
    }

    fn tempdir_stub() -> PathBuf {
        std::env::temp_dir()
    }
}
