// Input validation (§ 6) and statistical guardrails (§ 7).
//
// Two-tier model from gui_plan.md:
//   Block = run cannot proceed (severity::block)
//   Warn  = run can proceed, but the user should be aware (severity::warn)
//
// Finding codes use a two-letter prefix:
//   F = file-level, P = parseability, S = schema/types,
//   X = cross-file, D = design/contrast, G = statistical guardrail.

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::project::Contrast;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Severity {
    Block,
    Warn,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Finding {
    pub severity: Severity,
    pub code: String,
    pub category: String,
    pub message: String,
    #[serde(skip_serializing_if = "Option::is_none", default)]
    pub suggested_fix: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none", default)]
    pub context: Option<String>,
}

impl Finding {
    fn block(code: &str, category: &str, message: impl Into<String>) -> Self {
        Self {
            severity: Severity::Block,
            code: code.to_string(),
            category: category.to_string(),
            message: message.into(),
            suggested_fix: None,
            context: None,
        }
    }
    fn warn(code: &str, category: &str, message: impl Into<String>) -> Self {
        Self {
            severity: Severity::Warn,
            code: code.to_string(),
            category: category.to_string(),
            message: message.into(),
            suggested_fix: None,
            context: None,
        }
    }
    fn with_fix(mut self, fix: impl Into<String>) -> Self {
        self.suggested_fix = Some(fix.into());
        self
    }
    fn with_context(mut self, context: impl Into<String>) -> Self {
        self.context = Some(context.into());
        self
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct ValidationRequest {
    pub counts_path: PathBuf,
    pub metadata_path: PathBuf,
    pub design_formula: Option<String>,
    pub contrast: Contrast,
    #[serde(default)]
    pub ref_levels: BTreeMap<String, String>,
    pub condition_column: String,
    pub experiment_group: Option<String>,
    pub control_group: Option<String>,
    pub project_dir: Option<PathBuf>,
    /// Run the design/contrast checks (§ 6.4 + ref-level guardrail). The Data
    /// step sets this false — the contrast is configured later in Analyze, so
    /// reporting "experiment group not set" there is premature noise.
    #[serde(default = "default_true")]
    pub include_design_checks: bool,
}

fn default_true() -> bool {
    true
}

#[derive(Debug, Clone, Serialize)]
pub struct CountsSummary {
    pub gene_count: usize,
    pub sample_count: usize,
    pub gene_id_column: String,
    pub sample_columns: Vec<String>,
    pub delimiter: char,
}

#[derive(Debug, Clone, Serialize)]
pub struct MetadataSummary {
    pub sample_count: usize,
    pub sample_id_column: String,
    pub columns: Vec<String>,
    pub column_value_distributions: BTreeMap<String, BTreeMap<String, usize>>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ValidationResult {
    pub findings: Vec<Finding>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub counts_summary: Option<CountsSummary>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub metadata_summary: Option<MetadataSummary>,
}

/// Inspect a metadata file on its own (columns + per-column value
/// distributions) so the UI can drive factor / level dropdowns. See
/// gui_ux_improvements item 5.
pub fn metadata_summary(path: &Path) -> Result<MetadataSummary, String> {
    let mut findings = Vec::new();
    match inspect_metadata(path, &mut findings) {
        Some(s) => Ok(s),
        None => Err(findings
            .into_iter()
            .find(|f| f.severity == Severity::Block)
            .map(|f| f.message)
            .unwrap_or_else(|| "could not read metadata file".to_string())),
    }
}

pub fn validate(req: &ValidationRequest) -> ValidationResult {
    let mut findings: Vec<Finding> = Vec::new();

    let counts_path = resolve(&req.counts_path, req.project_dir.as_deref());
    let metadata_path = resolve(&req.metadata_path, req.project_dir.as_deref());

    let counts_summary = if file_check(&counts_path, "counts", &mut findings) {
        match inspect_counts(&counts_path, &mut findings) {
            Some(s) => Some(s),
            None => None,
        }
    } else {
        None
    };

    let metadata_summary = if file_check(&metadata_path, "metadata", &mut findings) {
        match inspect_metadata(&metadata_path, &mut findings) {
            Some(s) => Some(s),
            None => None,
        }
    } else {
        None
    };

    if let (Some(c), Some(m)) = (counts_summary.as_ref(), metadata_summary.as_ref()) {
        check_sample_alignment(c, m, &mut findings);
        if req.include_design_checks {
            check_design(m, req, &mut findings);
        }
    }

    ValidationResult {
        findings,
        counts_summary,
        metadata_summary,
    }
}

fn resolve(p: &Path, project_dir: Option<&Path>) -> PathBuf {
    if p.is_absolute() {
        return p.to_path_buf();
    }
    if let Some(dir) = project_dir {
        return dir.join(p);
    }
    p.to_path_buf()
}

fn file_check(path: &Path, role: &str, findings: &mut Vec<Finding>) -> bool {
    if !path.exists() {
        findings.push(
            Finding::block(
                "F001",
                "file",
                format!("{role} file not found"),
            )
            .with_context(path.display().to_string())
            .with_fix(format!(
                "Pick a valid {role} file from the input picker."
            )),
        );
        return false;
    }
    if !path.is_file() {
        findings.push(
            Finding::block(
                "F002",
                "file",
                format!("{role} path is not a regular file"),
            )
            .with_context(path.display().to_string()),
        );
        return false;
    }
    true
}

fn detect_delimiter(line: &str) -> char {
    let tabs = line.matches('\t').count();
    let commas = line.matches(',').count();
    if tabs > commas {
        '\t'
    } else {
        ','
    }
}

const GENE_ID_HEADER_HINTS: &[&str] = &["gene_id", "geneid", "gene", "id"];
const SAMPLE_ID_HEADER_HINTS: &[&str] = &["sample_id", "sample", "id"];

fn inspect_counts(path: &Path, findings: &mut Vec<Finding>) -> Option<CountsSummary> {
    let file = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            findings.push(
                Finding::block("F003", "file", format!("Cannot read counts file: {e}"))
                    .with_context(path.display().to_string()),
            );
            return None;
        }
    };
    let mut lines = BufReader::new(file).lines();
    let header = match lines.next() {
        Some(Ok(h)) => h,
        Some(Err(e)) => {
            findings.push(Finding::block("P001", "parse", format!("Counts: {e}")));
            return None;
        }
        None => {
            findings.push(Finding::block("P003", "parse", "Counts file is empty"));
            return None;
        }
    };

    let delimiter = detect_delimiter(&header);
    let header_cols: Vec<String> = header
        .split(delimiter)
        .map(|s| s.trim().trim_matches('"').to_string())
        .collect();
    if header_cols.len() < 2 {
        findings.push(Finding::block(
            "P003",
            "parse",
            "Counts header has fewer than 2 columns; expected gene_id + samples",
        ));
        return None;
    }

    let gene_id_idx = pick_gene_id_column(&header_cols).unwrap_or(0);
    let gene_id_column = header_cols[gene_id_idx].clone();
    let sample_columns: Vec<String> = header_cols
        .iter()
        .enumerate()
        .filter(|(i, _)| *i != gene_id_idx)
        .map(|(_, s)| s.clone())
        .collect();

    if sample_columns.is_empty() {
        findings.push(Finding::block(
            "S004",
            "schema",
            "Counts file has no sample columns",
        ));
    }

    // Header-level guardrail: gene_id column wasn't a known synonym.
    if !is_gene_id_header(&header_cols[gene_id_idx]) {
        findings.push(
            Finding::warn(
                "P005",
                "parse",
                format!(
                    "Could not identify a gene_id column; using first column \"{}\"",
                    header_cols[gene_id_idx]
                ),
            )
            .with_fix("Rename the gene identifier column to 'gene_id' or 'Geneid'."),
        );
    }

    // Full type scan: every data row is checked, not just a prefix, so the
    // Run gate matches what the FlashDEG CLI accepts. The CLI rejects any
    // non-integer / negative / non-numeric count (src/core/csv.cpp), so a bad
    // value in row 5000 must be caught here rather than at run time.
    let mut total_rows = 0usize;
    let mut seen_fractional = false;
    let mut seen_negative = false;
    let mut seen_non_numeric: Option<(usize, String, String)> = None;
    let mut gene_ids_seen: BTreeMap<String, usize> = BTreeMap::new();

    for (row_idx, line_res) in lines.enumerate() {
        let line = match line_res {
            Ok(l) => l,
            Err(_) => continue,
        };
        if line.trim().is_empty() {
            continue;
        }
        total_rows += 1;
        let cells: Vec<&str> = line.split(delimiter).collect();
        if cells.len() != header_cols.len() {
            findings.push(
                Finding::warn(
                    "P004",
                    "parse",
                    format!(
                        "Counts row {} has {} columns, expected {}",
                        row_idx + 2,
                        cells.len(),
                        header_cols.len()
                    ),
                )
                .with_context(path.display().to_string()),
            );
            continue;
        }
        if let Some(gid) = cells.get(gene_id_idx) {
            *gene_ids_seen.entry(gid.trim().to_string()).or_insert(0) += 1;
        }
        for (col_idx, raw) in cells.iter().enumerate() {
            if col_idx == gene_id_idx {
                continue;
            }
            let val = raw.trim().trim_matches('"');
            if val.is_empty() || val.eq_ignore_ascii_case("na") {
                continue;
            }
            match val.parse::<f64>() {
                Ok(v) => {
                    if v < 0.0 {
                        seen_negative = true;
                    }
                    if v.fract() != 0.0 {
                        seen_fractional = true;
                    }
                }
                Err(_) => {
                    if seen_non_numeric.is_none() {
                        seen_non_numeric = Some((
                            row_idx + 2,
                            header_cols[col_idx].clone(),
                            val.to_string(),
                        ));
                    }
                }
            }
        }
    }

    if let Some((row, col, value)) = seen_non_numeric {
        findings.push(
            Finding::block(
                "S001",
                "schema",
                format!("Counts column \"{col}\" contains non-numeric value \"{value}\""),
            )
            .with_context(format!("row {row}")),
        );
    }
    if seen_negative {
        findings.push(
            Finding::block(
                "S003",
                "schema",
                "Counts contain negative values; raw counts must be non-negative integers",
            )
            .with_fix("Use raw read counts (e.g. from featureCounts), not log-transformed values."),
        );
    }
    if seen_fractional {
        findings.push(
            Finding::block(
                "S002",
                "schema",
                "Counts contain fractional values; FlashDEG requires integer raw counts and will reject these",
            )
            .with_fix(
                "Looks like TPM / FPKM / normalised abundance. Use raw integer counts (e.g. from featureCounts).",
            ),
        );
    }

    // Duplicate gene IDs (full scan).
    for (gid, n) in &gene_ids_seen {
        if *n > 1 {
            findings.push(
                Finding::block(
                    "X008",
                    "schema",
                    format!("Duplicate gene ID \"{gid}\" in counts file"),
                )
                .with_context(format!("seen {n} times")),
            );
            break; // report the first; user fixes one at a time
        }
    }

    Some(CountsSummary {
        gene_count: total_rows,
        sample_count: sample_columns.len(),
        gene_id_column,
        sample_columns,
        delimiter,
    })
}

fn inspect_metadata(path: &Path, findings: &mut Vec<Finding>) -> Option<MetadataSummary> {
    let file = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            findings.push(
                Finding::block("F004", "file", format!("Cannot read metadata file: {e}"))
                    .with_context(path.display().to_string()),
            );
            return None;
        }
    };
    let mut lines = BufReader::new(file).lines();
    let header = match lines.next() {
        Some(Ok(h)) => h,
        Some(Err(e)) => {
            findings.push(Finding::block("P002", "parse", format!("Metadata: {e}")));
            return None;
        }
        None => {
            findings.push(Finding::block("P004", "parse", "Metadata file is empty"));
            return None;
        }
    };
    let delimiter = detect_delimiter(&header);
    let header_cols: Vec<String> = header
        .split(delimiter)
        .map(|s| s.trim().trim_matches('"').to_string())
        .collect();
    if header_cols.is_empty() {
        findings.push(Finding::block("P004", "parse", "Metadata header is empty"));
        return None;
    }
    let sample_id_idx = pick_sample_id_column(&header_cols).unwrap_or(0);
    let sample_id_column = header_cols[sample_id_idx].clone();

    if !is_sample_id_header(&sample_id_column) {
        findings.push(
            Finding::warn(
                "P006",
                "parse",
                format!(
                    "Could not identify a sample_id column; using first column \"{sample_id_column}\""
                ),
            )
            .with_fix("Rename the sample identifier column to 'sample_id' or 'sample'."),
        );
    }

    let mut sample_ids: Vec<String> = Vec::new();
    let mut column_value_distributions: BTreeMap<String, BTreeMap<String, usize>> =
        BTreeMap::new();
    let mut id_seen: BTreeMap<String, usize> = BTreeMap::new();

    for line_res in lines {
        let line = match line_res {
            Ok(l) => l,
            Err(_) => continue,
        };
        if line.trim().is_empty() {
            continue;
        }
        // Strip quotes but NOT whitespace: trailing spaces in sample IDs
        // and category values are real bugs that we want to surface via
        // the X003/G003 guardrails. Compare loosely later, store strictly
        // now.
        let cells: Vec<String> = line
            .split(delimiter)
            .map(|s| s.trim_matches('"').to_string())
            .collect();
        if cells.len() != header_cols.len() {
            continue;
        }
        let sid = cells.get(sample_id_idx).cloned().unwrap_or_default();
        if sid.is_empty() {
            continue;
        }
        *id_seen.entry(sid.clone()).or_insert(0) += 1;
        sample_ids.push(sid);
        // Populate distributions for every column, including the sample_id
        // column itself. check_sample_alignment looks the sample_id column
        // up here to discover the set of metadata sample IDs.
        for (i, val) in cells.iter().enumerate() {
            let col_name = &header_cols[i];
            column_value_distributions
                .entry(col_name.clone())
                .or_default()
                .entry(val.clone())
                .and_modify(|v| *v += 1)
                .or_insert(1);
        }
    }

    for (sid, n) in &id_seen {
        if *n > 1 {
            findings.push(
                Finding::block(
                    "X007",
                    "schema",
                    format!("Duplicate sample ID \"{sid}\" in metadata"),
                )
                .with_context(format!("seen {n} times")),
            );
            break;
        }
    }

    // Whitespace / case typo detection per column.
    for (col, dist) in &column_value_distributions {
        let mut bins: BTreeMap<String, Vec<String>> = BTreeMap::new();
        for v in dist.keys() {
            let norm = v.trim().to_ascii_lowercase();
            bins.entry(norm).or_default().push(v.clone());
        }
        for (norm, variants) in bins {
            if variants.len() > 1 {
                findings.push(
                    Finding::warn(
                        "G003",
                        "guardrail",
                        format!(
                            "Metadata column \"{col}\" has values that look like typos of each other: {}",
                            variants.iter().map(|v| format!("{v:?}")).collect::<Vec<_>>().join(", ")
                        ),
                    )
                    .with_fix(format!(
                        "Normalise these to a single value (e.g. \"{norm}\")."
                    )),
                );
            }
        }
    }

    let columns = header_cols.clone();
    Some(MetadataSummary {
        sample_count: sample_ids.len(),
        sample_id_column,
        columns,
        column_value_distributions,
    })
}

fn check_sample_alignment(
    counts: &CountsSummary,
    metadata: &MetadataSummary,
    findings: &mut Vec<Finding>,
) {
    let counts_set: BTreeMap<String, ()> =
        counts.sample_columns.iter().map(|s| (s.clone(), ())).collect();
    let metadata_ids: Vec<String> = metadata
        .column_value_distributions
        .get(&metadata.sample_id_column)
        .map(|d| d.keys().cloned().collect())
        .unwrap_or_default();

    // First pass: exact match.
    let metadata_set: BTreeMap<String, ()> = metadata_ids.iter().map(|s| (s.clone(), ())).collect();
    let in_counts_only: Vec<&String> = counts_set
        .keys()
        .filter(|s| !metadata_set.contains_key(*s))
        .collect();
    let in_metadata_only: Vec<&String> = metadata_set
        .keys()
        .filter(|s| !counts_set.contains_key(*s))
        .collect();

    if in_counts_only.is_empty() && in_metadata_only.is_empty() {
        return;
    }

    // Build a fuzzy index over metadata IDs to suggest alignment fixes.
    let metadata_normalised: BTreeMap<String, Vec<String>> = {
        let mut m: BTreeMap<String, Vec<String>> = BTreeMap::new();
        for id in &metadata_ids {
            for key in fuzzy_keys(id) {
                m.entry(key).or_default().push(id.clone());
            }
        }
        m
    };

    let mut auto_fix_suggestions: Vec<(String, String, &'static str)> = Vec::new();
    let mut still_unmatched_in_counts: Vec<&String> = Vec::new();

    for c in &in_counts_only {
        let mut matched = None;
        for key in fuzzy_keys(c) {
            if let Some(candidates) = metadata_normalised.get(&key) {
                if let Some(cand) = candidates.first() {
                    if cand != *c {
                        let reason = fuzzy_reason(c, cand);
                        matched = Some((cand.clone(), reason));
                        break;
                    }
                }
            }
        }
        match matched {
            Some((cand, reason)) => {
                auto_fix_suggestions.push(((*c).clone(), cand, reason));
            }
            None => still_unmatched_in_counts.push(c),
        }
    }

    for (raw, suggestion, reason) in &auto_fix_suggestions {
        let code = match *reason {
            "whitespace" => "X003",
            "case" => "X004",
            "full_half_width" => "X005",
            _ => "X001",
        };
        findings.push(
            Finding::warn(
                code,
                "cross_file",
                format!(
                    "Counts column \"{raw}\" looks like metadata sample \"{suggestion}\" with {reason} difference"
                ),
            )
            .with_fix(format!("Rename one side so they match exactly.")),
        );
    }

    if !still_unmatched_in_counts.is_empty() {
        findings.push(
            Finding::block(
                "X001",
                "cross_file",
                format!(
                    "{} sample column(s) in counts have no matching metadata row",
                    still_unmatched_in_counts.len()
                ),
            )
            .with_context(
                still_unmatched_in_counts
                    .iter()
                    .take(5)
                    .map(|s| s.as_str())
                    .collect::<Vec<_>>()
                    .join(", "),
            ),
        );
    }
    if !in_metadata_only.is_empty() {
        findings.push(
            Finding::block(
                "X002",
                "cross_file",
                format!(
                    "{} metadata sample(s) have no matching counts column",
                    in_metadata_only.len()
                ),
            )
            .with_context(
                in_metadata_only
                    .iter()
                    .take(5)
                    .map(|s| s.as_str())
                    .collect::<Vec<_>>()
                    .join(", "),
            ),
        );
    }
}

fn check_design(
    metadata: &MetadataSummary,
    req: &ValidationRequest,
    findings: &mut Vec<Finding>,
) {
    let columns: Vec<&String> = metadata
        .column_value_distributions
        .keys()
        .collect();

    let condition = req.condition_column.trim();
    if condition.is_empty() {
        findings.push(Finding::block(
            "D001",
            "design",
            "condition column was not specified",
        ));
        return;
    }
    if !columns.iter().any(|c| c.as_str() == condition) {
        findings.push(
            Finding::block(
                "D002",
                "design",
                format!("metadata has no column named \"{condition}\""),
            )
            .with_fix(format!(
                "Available columns: {}",
                columns.iter().map(|c| c.as_str()).collect::<Vec<_>>().join(", ")
            )),
        );
        return;
    }
    let condition_values = metadata
        .column_value_distributions
        .get(condition)
        .cloned()
        .unwrap_or_default();
    let levels: Vec<&String> = condition_values.keys().collect();
    if levels.len() < 2 {
        findings.push(Finding::block(
            "D007",
            "design",
            format!(
                "condition column \"{condition}\" has fewer than 2 distinct values"
            ),
        ));
        return;
    }

    if let (Some(exp), Some(ctrl)) = (req.experiment_group.as_deref(), req.control_group.as_deref()) {
        if exp == ctrl {
            findings.push(Finding::block(
                "D003",
                "design",
                "experiment and control groups must differ",
            ));
        }
        if !condition_values.contains_key(exp) {
            findings.push(
                Finding::block(
                    "D003",
                    "design",
                    format!(
                        "experiment group \"{exp}\" not in condition column \"{condition}\""
                    ),
                )
                .with_fix(format!(
                    "Available levels: {}",
                    levels.iter().map(|l| l.as_str()).collect::<Vec<_>>().join(", ")
                )),
            );
        }
        if !condition_values.contains_key(ctrl) {
            findings.push(
                Finding::block(
                    "D004",
                    "design",
                    format!(
                        "control group \"{ctrl}\" not in condition column \"{condition}\""
                    ),
                )
                .with_fix(format!(
                    "Available levels: {}",
                    levels.iter().map(|l| l.as_str()).collect::<Vec<_>>().join(", ")
                )),
            );
        }

        // Replicate count per group.
        if let Some(&n_exp) = condition_values.get(exp) {
            if n_exp < 2 {
                findings.push(Finding::block(
                    "D008",
                    "design",
                    format!(
                        "experiment group \"{exp}\" has only {n_exp} replicate; DESeq2 needs at least 2"
                    ),
                ));
            }
        }
        if let Some(&n_ctrl) = condition_values.get(ctrl) {
            if n_ctrl < 2 {
                findings.push(Finding::block(
                    "D008",
                    "design",
                    format!(
                        "control group \"{ctrl}\" has only {n_ctrl} replicate; DESeq2 needs at least 2"
                    ),
                ));
            }
        }

        // Ref-level guardrail: warn if ref_levels doesn't pin the control,
        // because DESeq2 would otherwise pick alphabetically.
        let pinned = req.ref_levels.get(condition).map(|s| s.as_str());
        match pinned {
            Some(v) if v == ctrl => { /* good */ }
            Some(v) => {
                findings.push(
                    Finding::warn(
                        "G001",
                        "guardrail",
                        format!(
                            "ref_level for \"{condition}\" is \"{v}\" but control is \"{ctrl}\""
                        ),
                    )
                    .with_fix(format!("Set ref_level[\"{condition}\"] = \"{ctrl}\".")),
                );
            }
            None => {
                let alphabetic_first = levels.first().map(|s| s.as_str()).unwrap_or("");
                if alphabetic_first != ctrl {
                    findings.push(
                        Finding::warn(
                            "G001",
                            "guardrail",
                            format!(
                                "ref_level for \"{condition}\" is not set; DESeq2 will pick \"{alphabetic_first}\" alphabetically, not \"{ctrl}\""
                            ),
                        )
                        .with_fix(format!("Set ref_level[\"{condition}\"] = \"{ctrl}\".")),
                    );
                } else {
                    findings.push(
                        Finding::warn(
                            "G001",
                            "guardrail",
                            format!(
                                "ref_level for \"{condition}\" is not set; relying on alphabetic order (\"{alphabetic_first}\" first)"
                            ),
                        )
                        .with_fix(format!("Set ref_level[\"{condition}\"] = \"{ctrl}\" to be explicit.")),
                    );
                }
            }
        }
    }

    // Verify any ref_levels referenced columns/values exist.
    for (col, level) in &req.ref_levels {
        let dist = match metadata.column_value_distributions.get(col) {
            Some(d) => d,
            None => {
                findings.push(Finding::block(
                    "D005",
                    "design",
                    format!("ref_level column \"{col}\" not in metadata"),
                ));
                continue;
            }
        };
        if !dist.contains_key(level) {
            findings.push(Finding::block(
                "D006",
                "design",
                format!("ref_level value \"{level}\" not found in column \"{col}\""),
            ));
        }
    }

    // Verify factor_levels contrast pieces (in addition to experiment/control).
    if let Contrast::FactorLevels {
        factor,
        test,
        control,
    } = &req.contrast
    {
        if let Some(dist) = metadata.column_value_distributions.get(factor) {
            if !dist.contains_key(test) {
                findings.push(Finding::block(
                    "D003",
                    "design",
                    format!("contrast test level \"{test}\" not in factor \"{factor}\""),
                ));
            }
            if !dist.contains_key(control) {
                findings.push(Finding::block(
                    "D004",
                    "design",
                    format!("contrast control level \"{control}\" not in factor \"{factor}\""),
                ));
            }
        } else {
            findings.push(Finding::block(
                "D002",
                "design",
                format!("contrast factor \"{factor}\" not in metadata"),
            ));
        }
    }
}

// ---------- helpers ----------

fn pick_gene_id_column(headers: &[String]) -> Option<usize> {
    for (i, h) in headers.iter().enumerate() {
        if is_gene_id_header(h) {
            return Some(i);
        }
    }
    None
}

fn pick_sample_id_column(headers: &[String]) -> Option<usize> {
    for (i, h) in headers.iter().enumerate() {
        if is_sample_id_header(h) {
            return Some(i);
        }
    }
    None
}

fn is_gene_id_header(s: &str) -> bool {
    let lower = s.trim().to_ascii_lowercase();
    GENE_ID_HEADER_HINTS.contains(&lower.as_str())
}

fn is_sample_id_header(s: &str) -> bool {
    let lower = s.trim().to_ascii_lowercase();
    SAMPLE_ID_HEADER_HINTS.contains(&lower.as_str())
}

/// Returns normalisation keys for fuzzy matching. The order matters: the
/// caller picks the first match, so cheaper fixes win.
fn fuzzy_keys(s: &str) -> Vec<String> {
    let trimmed = s.trim().to_string();
    let lower = trimmed.to_ascii_lowercase();
    let half = full_to_half_width(&trimmed);
    let half_lower = half.to_ascii_lowercase();
    let mut keys = Vec::with_capacity(4);
    keys.push(trimmed.clone());
    if lower != trimmed {
        keys.push(lower);
    }
    if half != trimmed {
        keys.push(half);
    }
    if half_lower != trimmed {
        keys.push(half_lower);
    }
    keys
}

fn fuzzy_reason(raw: &str, target: &str) -> &'static str {
    if raw.trim() == target.trim() && raw != target {
        return "whitespace";
    }
    if raw.to_ascii_lowercase() == target.to_ascii_lowercase() && raw != target {
        return "case";
    }
    if full_to_half_width(raw) == full_to_half_width(target) && raw != target {
        return "full_half_width";
    }
    "unknown"
}

/// Convert full-width ASCII (Unicode block FF00–FF5E) to ASCII. Used to
/// catch the common Japanese-input error of mixing 'gene_001' and
/// 'ｇｅｎｅ_001'.
fn full_to_half_width(s: &str) -> String {
    s.chars()
        .map(|c| {
            let code = c as u32;
            if (0xFF01..=0xFF5E).contains(&code) {
                char::from_u32(code - 0xFEE0).unwrap_or(c)
            } else {
                c
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::sync::atomic::{AtomicUsize, Ordering};

    // Per-call counter avoids races between parallel tests that would
    // otherwise share a filename derived only from the `name` argument.
    static FIXTURE_NONCE: AtomicUsize = AtomicUsize::new(0);

    fn temp_csv(name: &str, content: &str) -> PathBuf {
        let nonce = FIXTURE_NONCE.fetch_add(1, Ordering::SeqCst);
        let dir = std::env::temp_dir().join(format!(
            "flashdeg_gui_validation_{}_{}_{}",
            std::process::id(),
            nonce,
            name
        ));
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("file.csv");
        let mut f = std::fs::File::create(&path).unwrap();
        f.write_all(content.as_bytes()).unwrap();
        path
    }

    fn factor_contrast() -> Contrast {
        Contrast::FactorLevels {
            factor: "condition".to_string(),
            test: "treated".to_string(),
            control: "control".to_string(),
        }
    }

    fn req(counts: PathBuf, metadata: PathBuf) -> ValidationRequest {
        ValidationRequest {
            counts_path: counts,
            metadata_path: metadata,
            design_formula: Some("~ condition".to_string()),
            contrast: factor_contrast(),
            ref_levels: {
                let mut m = BTreeMap::new();
                m.insert("condition".to_string(), "control".to_string());
                m
            },
            condition_column: "condition".to_string(),
            experiment_group: Some("treated".to_string()),
            control_group: Some("control".to_string()),
            project_dir: None,
            include_design_checks: true,
        }
    }

    fn write_ok_counts() -> PathBuf {
        temp_csv(
            "ok_counts",
            "gene_id,s1,s2,s3,s4\n\
             gene1,100,200,150,180\n\
             gene2,50,60,55,58\n",
        )
    }

    fn write_ok_metadata() -> PathBuf {
        temp_csv(
            "ok_meta",
            "sample_id,condition\n\
             s1,control\n\
             s2,control\n\
             s3,treated\n\
             s4,treated\n",
        )
    }

    #[test]
    fn clean_inputs_produce_no_blockers() {
        let r = req(write_ok_counts(), write_ok_metadata());
        let result = validate(&r);
        let blocks: Vec<_> = result.findings.iter().filter(|f| f.severity == Severity::Block).collect();
        assert!(blocks.is_empty(), "unexpected blocks: {blocks:?}");
    }

    #[test]
    fn detects_missing_counts_file() {
        let r = req(PathBuf::from("/no/such/file.csv"), write_ok_metadata());
        let result = validate(&r);
        let has_f001 = result.findings.iter().any(|f| f.code == "F001");
        assert!(has_f001, "expected F001 in {:?}", result.findings);
    }

    #[test]
    fn detects_negative_counts() {
        let counts = temp_csv(
            "neg_counts",
            "gene_id,s1,s2,s3,s4\n\
             gene1,100,-5,150,180\n",
        );
        let r = req(counts, write_ok_metadata());
        let result = validate(&r);
        assert!(result.findings.iter().any(|f| f.code == "S003"));
    }

    #[test]
    fn detects_fractional_counts() {
        let counts = temp_csv(
            "frac_counts",
            "gene_id,s1,s2,s3,s4\n\
             gene1,100.5,200,150,180\n",
        );
        let r = req(counts, write_ok_metadata());
        let result = validate(&r);
        let f = result.findings.iter().find(|f| f.code == "S002").expect("S002 missing");
        assert_eq!(f.severity, Severity::Block);
    }

    #[test]
    fn full_scan_catches_bad_value_past_200_rows() {
        // A fractional value at row ~250 must still be caught (the old code
        // capped type-checking at 200 rows). Header + 300 data rows.
        let mut content = String::from("gene_id,s1,s2,s3,s4\n");
        for i in 0..300 {
            if i == 250 {
                content.push_str(&format!("gene{i},10,20.5,30,40\n"));
            } else {
                content.push_str(&format!("gene{i},10,20,30,40\n"));
            }
        }
        let counts = temp_csv("big_counts", &content);
        let r = req(counts, write_ok_metadata());
        let result = validate(&r);
        let f = result.findings.iter().find(|f| f.code == "S002").expect("S002 missing");
        assert_eq!(f.severity, Severity::Block);
    }

    #[test]
    fn detects_sample_name_mismatch_with_case_suggestion() {
        let counts = temp_csv(
            "case_mismatch",
            "gene_id,S1,S2,s3,s4\n\
             gene1,1,2,3,4\n",
        );
        let r = req(counts, write_ok_metadata());
        let result = validate(&r);
        let case_fix = result.findings.iter().any(|f| f.code == "X004");
        assert!(case_fix, "expected X004 case fix: {:?}", result.findings);
    }

    #[test]
    fn detects_sample_name_mismatch_with_whitespace_suggestion() {
        let metadata = temp_csv(
            "ws_meta",
            "sample_id,condition\n\
             s1 ,control\n\
             s2,control\n\
             s3,treated\n\
             s4,treated\n",
        );
        let r = req(write_ok_counts(), metadata);
        let result = validate(&r);
        let ws_fix = result.findings.iter().any(|f| f.code == "X003");
        assert!(ws_fix, "expected X003 whitespace fix: {:?}", result.findings);
    }

    #[test]
    fn detects_missing_condition_column() {
        let metadata = temp_csv(
            "no_cond",
            "sample_id,batch\n\
             s1,A\n\
             s2,A\n\
             s3,B\n\
             s4,B\n",
        );
        let r = req(write_ok_counts(), metadata);
        let result = validate(&r);
        assert!(result.findings.iter().any(|f| f.code == "D002"));
    }

    #[test]
    fn detects_wrong_ref_level_relative_to_control() {
        let mut r = req(write_ok_counts(), write_ok_metadata());
        r.ref_levels.insert("condition".to_string(), "treated".to_string());
        let result = validate(&r);
        assert!(result.findings.iter().any(|f| f.code == "G001"));
    }

    #[test]
    fn detects_low_replicate_count() {
        let metadata = temp_csv(
            "single_rep",
            "sample_id,condition\n\
             s1,control\n\
             s2,treated\n\
             s3,treated\n\
             s4,treated\n",
        );
        let counts = temp_csv(
            "single_rep_counts",
            "gene_id,s1,s2,s3,s4\n\
             gene1,1,2,3,4\n",
        );
        let r = req(counts, metadata);
        let result = validate(&r);
        let d008 = result.findings.iter().any(|f| f.code == "D008");
        assert!(d008, "expected D008, got {:?}", result.findings);
    }

    #[test]
    fn detects_typo_variants_in_metadata_column() {
        let metadata = temp_csv(
            "typo_meta",
            "sample_id,condition\n\
             s1,control\n\
             s2,control\n\
             s3,treated\n\
             s4,treated \n",
        );
        let r = req(write_ok_counts(), metadata);
        let result = validate(&r);
        let g003 = result.findings.iter().any(|f| f.code == "G003");
        assert!(g003, "expected G003, got {:?}", result.findings);
    }
}
