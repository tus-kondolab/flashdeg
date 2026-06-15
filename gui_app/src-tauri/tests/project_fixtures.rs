// Integration tests for the project file v1 schema. Each fixture must
// round-trip through serde without drift. See gui_plan.md § 17.

use std::path::PathBuf;

use flashdeg_gui_lib::project::{self, Contrast, Design, Project};

fn fixture(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join(name)
}

fn load_normalized(path: PathBuf) -> serde_json::Value {
    let project = project::load(&path).expect("load fixture");
    serde_json::to_value(&project).expect("re-serialize")
}

#[test]
fn factor_levels_fixture_round_trips() {
    let v = load_normalized(fixture("project_v1_factor_levels.flashdeg"));
    assert_eq!(v["schema_version"], 1);
    assert_eq!(v["contrast"]["kind"], "factor_levels");
    assert_eq!(v["contrast"]["factor"], "condition");
    assert_eq!(v["contrast"]["test"], "treated");
    assert_eq!(v["contrast"]["control"], "control");
    assert_eq!(v["design"]["kind"], "formula");
}

#[test]
fn design_column_fixture_round_trips() {
    let p = project::load(&fixture("project_v1_design_column.flashdeg")).unwrap();
    match &p.contrast {
        Contrast::DesignColumn { name } => {
            assert_eq!(name, "genotype[T.KO]:treatment[T.drug]");
        }
        _ => panic!("expected design_column contrast"),
    }
    match &p.design {
        Design::Formula { formula } => {
            assert_eq!(formula, "~ genotype * treatment");
        }
        _ => panic!("expected formula design"),
    }
}

#[test]
fn vector_contrast_with_matrix_design_round_trips() {
    let p = project::load(&fixture("project_v1_vector.flashdeg")).unwrap();
    match &p.contrast {
        Contrast::Vector { values } => assert_eq!(values, &vec![0.0, -1.0, 1.0]),
        _ => panic!("expected vector contrast"),
    }
    match &p.design {
        Design::Matrix { columns, .. } => {
            assert_eq!(columns.len(), 3);
            assert_eq!(columns[0], "Intercept");
        }
        _ => panic!("expected matrix design"),
    }
}

#[test]
fn frozen_factor_levels_snapshot() {
    // Byte-equal snapshot guard. If this fails after a serde change,
    // either the schema actually changed (then bump schema_version and
    // update the fixture intentionally) or the change is unintentional.
    let path = fixture("project_v1_factor_levels.flashdeg");
    let on_disk = std::fs::read_to_string(&path).expect("read fixture");
    let parsed: Project = serde_json::from_str(&on_disk).expect("parse fixture");
    let re_serialized = serde_json::to_string_pretty(&parsed).expect("serialize");
    let normalize = |s: &str| s.replace("\r\n", "\n").trim_end().to_string();
    assert_eq!(normalize(&on_disk), normalize(&re_serialized));
}
