// featureCounts import. See gui_plan.md § 5.2 and § 6.5.
//
// Subread's `featureCounts` writes a TSV with optional `#`-prefixed comment
// lines at the top, then a header row of the form
//
//   Geneid Chr Start End Strand Length <sample1.bam> [<sample2.bam> ...]
//
// One file may carry many sample columns. We support that as well as the
// common one-file-per-sample layout. Sample names are inferred from each
// count column header (basename without `.bam` / `.sam`); users can override
// before the merge.

use std::collections::{BTreeMap, BTreeSet};
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

const ANNOTATION_COLUMNS: &[&str] = &["Chr", "Start", "End", "Strand", "Length"];
const GENEID_HINTS: &[&str] = &["Geneid", "GeneID", "gene_id", "Gene", "gene"];

#[derive(Debug, Clone, Serialize)]
pub struct SampleColumn {
    pub raw_header: String,
    pub inferred_sample_name: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct FeatureCountsInspection {
    pub path: String,
    /// "\t" or "," — encoded as a string so it survives JSON IPC.
    pub delimiter: String,
    pub geneid_column: String,
    pub annotation_columns: Vec<String>,
    pub sample_columns: Vec<SampleColumn>,
    pub data_row_count: usize,
}

#[derive(Debug, thiserror::Error)]
pub enum FCError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("CSV write error: {0}")]
    Csv(#[from] csv::Error),
    #[error("featureCounts file has no header row")]
    NoHeader,
    #[error("featureCounts file is missing a Geneid column")]
    MissingGeneid,
    #[error("featureCounts file has no sample columns")]
    NoSampleColumns,
    #[error("merge requested with no files")]
    NoFiles,
    #[error("Duplicate sample name across files: {0}")]
    DuplicateSample(String),
    #[error("Duplicate gene ID in {file}: {gene}")]
    DuplicateGene { file: String, gene: String },
    #[error("Gene sets differ between files but policy is `error`")]
    GeneSetMismatch,
    #[error("Row {row} in {file} has {got} columns, expected {expected}")]
    BadRow {
        file: String,
        row: usize,
        got: usize,
        expected: usize,
    },
    #[error("Cannot parse count {value:?} in {file} (row {row}, column {column})")]
    BadCount {
        file: String,
        row: usize,
        column: String,
        value: String,
    },
    #[error("mismatch between sample_names and rows in metadata write request")]
    MetadataLengthMismatch,
}

pub fn inspect(path: &Path) -> Result<FeatureCountsInspection, FCError> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut lines = reader.lines();

    let header_line = read_first_data_line(&mut lines)?;
    let delimiter = detect_delimiter(&header_line);
    let columns = parse_columns(&header_line, delimiter);

    let geneid_idx = columns
        .iter()
        .position(|c| GENEID_HINTS.contains(&c.as_str()))
        .ok_or(FCError::MissingGeneid)?;
    let geneid_column = columns[geneid_idx].clone();

    let mut annotation_columns = Vec::new();
    let mut sample_columns = Vec::new();
    for (i, col) in columns.iter().enumerate() {
        if i == geneid_idx {
            continue;
        }
        if ANNOTATION_COLUMNS.contains(&col.as_str()) {
            annotation_columns.push(col.clone());
        } else {
            sample_columns.push(SampleColumn {
                raw_header: col.clone(),
                inferred_sample_name: infer_sample_name(col),
            });
        }
    }

    if sample_columns.is_empty() {
        return Err(FCError::NoSampleColumns);
    }

    let mut data_row_count = 0usize;
    for line in lines {
        let Ok(line) = line else { continue };
        if line.trim().is_empty() || line.starts_with('#') {
            continue;
        }
        data_row_count += 1;
    }

    Ok(FeatureCountsInspection {
        path: path.display().to_string(),
        delimiter: if delimiter == '\t' {
            "\\t".to_string()
        } else {
            delimiter.to_string()
        },
        geneid_column,
        annotation_columns,
        sample_columns,
        data_row_count,
    })
}

#[derive(Debug, Clone, Deserialize)]
pub struct FileSpec {
    pub path: PathBuf,
    /// raw_header -> overridden sample name
    #[serde(default)]
    pub sample_overrides: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum GeneSetDiffPolicy {
    /// Refuse the merge if gene sets differ across files.
    Error,
    /// Use the intersection of gene sets; warn how many genes were dropped.
    Intersection,
    /// Use the union of gene sets; fill missing cells with 0.
    Union,
}

#[derive(Debug, Clone, Deserialize)]
pub struct MergeRequest {
    pub files: Vec<FileSpec>,
    pub output_counts_path: PathBuf,
    #[serde(default = "default_policy")]
    pub on_gene_set_diff: GeneSetDiffPolicy,
}

fn default_policy() -> GeneSetDiffPolicy {
    GeneSetDiffPolicy::Intersection
}

#[derive(Debug, Clone, Serialize)]
pub struct MergeReport {
    pub output_path: String,
    pub gene_count: usize,
    pub sample_count: usize,
    pub sample_names: Vec<String>,
    pub warnings: Vec<String>,
}

pub fn merge_and_write(req: &MergeRequest) -> Result<MergeReport, FCError> {
    if req.files.is_empty() {
        return Err(FCError::NoFiles);
    }

    let mut all_per_file: Vec<PerFileData> = Vec::with_capacity(req.files.len());
    let mut sample_name_order: Vec<String> = Vec::new();
    let mut sample_name_set: BTreeSet<String> = BTreeSet::new();

    for spec in &req.files {
        let data = parse_file(&spec.path, &spec.sample_overrides)?;
        for s in &data.sample_names {
            if !sample_name_set.insert(s.clone()) {
                return Err(FCError::DuplicateSample(s.clone()));
            }
            sample_name_order.push(s.clone());
        }
        all_per_file.push(data);
    }

    let mut warnings: Vec<String> = Vec::new();
    let gene_sets: Vec<BTreeSet<&str>> = all_per_file
        .iter()
        .map(|d| d.gene_ids.iter().map(String::as_str).collect())
        .collect();

    let gene_set: Vec<String> = match req.on_gene_set_diff {
        GeneSetDiffPolicy::Intersection => {
            let mut iter = gene_sets.iter().cloned();
            let first = iter.next().unwrap_or_default();
            let inter: BTreeSet<&str> =
                iter.fold(first, |acc, s| acc.intersection(&s).copied().collect());
            let max_per_file = all_per_file
                .iter()
                .map(|d| d.gene_ids.len())
                .max()
                .unwrap_or(0);
            let lost = max_per_file.saturating_sub(inter.len());
            if lost > 0 {
                warnings.push(format!(
                    "intersection dropped {lost} gene(s) not present in every file"
                ));
            }
            inter.into_iter().map(str::to_string).collect()
        }
        GeneSetDiffPolicy::Union => {
            let mut all: BTreeSet<&str> = BTreeSet::new();
            for s in &gene_sets {
                all.extend(s.iter().copied());
            }
            let any_diff = gene_sets
                .iter()
                .any(|s| s != gene_sets.first().unwrap_or(&BTreeSet::new()));
            if any_diff {
                warnings.push(
                    "union policy: some gene IDs are not present in every file; missing cells set to 0".into(),
                );
            }
            all.into_iter().map(str::to_string).collect()
        }
        GeneSetDiffPolicy::Error => {
            let first = gene_sets.first().cloned().unwrap_or_default();
            if !gene_sets.iter().all(|s| s == &first) {
                return Err(FCError::GeneSetMismatch);
            }
            first.into_iter().map(str::to_string).collect()
        }
    };

    // Use csv::Writer so sample names / gene IDs containing commas or quotes
    // are escaped (mirrors the CLI's write_csv_escaped in src/core/csv.cpp).
    let mut wtr = csv::Writer::from_path(&req.output_counts_path)?;
    let mut header: Vec<String> = Vec::with_capacity(sample_name_order.len() + 1);
    header.push("gene_id".to_string());
    header.extend(sample_name_order.iter().cloned());
    wtr.write_record(&header)?;

    for gene in &gene_set {
        let mut record: Vec<String> = Vec::with_capacity(sample_name_order.len() + 1);
        record.push(gene.clone());
        for data in &all_per_file {
            for sample in &data.sample_names {
                let v = data
                    .counts
                    .get(gene)
                    .and_then(|row| row.get(sample))
                    .copied()
                    .unwrap_or(0);
                record.push(v.to_string());
            }
        }
        wtr.write_record(&record)?;
    }
    wtr.flush()?;

    Ok(MergeReport {
        output_path: req.output_counts_path.display().to_string(),
        gene_count: gene_set.len(),
        sample_count: sample_name_order.len(),
        sample_names: sample_name_order,
        warnings,
    })
}

struct PerFileData {
    gene_ids: Vec<String>,
    sample_names: Vec<String>,
    counts: BTreeMap<String, BTreeMap<String, u64>>,
}

fn parse_file(
    path: &Path,
    overrides: &BTreeMap<String, String>,
) -> Result<PerFileData, FCError> {
    let file = File::open(path)?;
    let reader = BufReader::new(file);
    let mut lines = reader.lines();

    let header_line = read_first_data_line(&mut lines)?;
    let delimiter = detect_delimiter(&header_line);
    let columns = parse_columns(&header_line, delimiter);

    let geneid_idx = columns
        .iter()
        .position(|c| GENEID_HINTS.contains(&c.as_str()))
        .ok_or(FCError::MissingGeneid)?;

    let mut sample_indices: Vec<(usize, String)> = Vec::new();
    let mut sample_names: Vec<String> = Vec::new();
    for (i, col) in columns.iter().enumerate() {
        if i == geneid_idx || ANNOTATION_COLUMNS.contains(&col.as_str()) {
            continue;
        }
        let sample_name = overrides
            .get(col)
            .cloned()
            .unwrap_or_else(|| infer_sample_name(col));
        sample_indices.push((i, sample_name.clone()));
        sample_names.push(sample_name);
    }

    if sample_indices.is_empty() {
        return Err(FCError::NoSampleColumns);
    }

    let mut gene_ids: Vec<String> = Vec::new();
    let mut counts: BTreeMap<String, BTreeMap<String, u64>> = BTreeMap::new();
    let path_str = path.display().to_string();

    for (row_idx, line) in lines.enumerate() {
        let line = match line {
            Ok(l) => l,
            Err(e) => return Err(FCError::Io(e)),
        };
        if line.trim().is_empty() || line.starts_with('#') {
            continue;
        }
        let cells: Vec<&str> = line.split(delimiter).collect();
        if cells.len() != columns.len() {
            return Err(FCError::BadRow {
                file: path_str.clone(),
                row: row_idx + 2,
                got: cells.len(),
                expected: columns.len(),
            });
        }
        let gene_id = cells[geneid_idx].trim().to_string();
        if counts.contains_key(&gene_id) {
            return Err(FCError::DuplicateGene {
                file: path_str.clone(),
                gene: gene_id,
            });
        }
        let mut row: BTreeMap<String, u64> = BTreeMap::new();
        for (i, sample) in &sample_indices {
            let raw = cells[*i].trim();
            let v: u64 = raw.parse().map_err(|_| FCError::BadCount {
                file: path_str.clone(),
                row: row_idx + 2,
                column: columns[*i].clone(),
                value: raw.to_string(),
            })?;
            row.insert(sample.clone(), v);
        }
        counts.insert(gene_id.clone(), row);
        gene_ids.push(gene_id);
    }

    Ok(PerFileData {
        gene_ids,
        sample_names,
        counts,
    })
}

#[derive(Debug, Clone, Deserialize)]
pub struct WriteMetadataRequest {
    pub output_path: PathBuf,
    pub sample_id_column: String,
    pub sample_names: Vec<String>,
    pub columns: Vec<String>,
    /// `rows[i]` is the value list for `sample_names[i]`, parallel to `columns`.
    pub rows: Vec<Vec<String>>,
}

#[derive(Debug, Clone, Serialize)]
pub struct WriteMetadataReport {
    pub output_path: String,
    pub sample_count: usize,
    pub column_count: usize,
}

pub fn write_metadata(req: &WriteMetadataRequest) -> Result<WriteMetadataReport, FCError> {
    if req.sample_names.len() != req.rows.len() {
        return Err(FCError::MetadataLengthMismatch);
    }
    let mut wtr = csv::Writer::from_path(&req.output_path)?;
    let mut header: Vec<String> = Vec::with_capacity(req.columns.len() + 1);
    header.push(req.sample_id_column.clone());
    header.extend(req.columns.iter().cloned());
    wtr.write_record(&header)?;
    for (i, sample) in req.sample_names.iter().enumerate() {
        let mut record: Vec<String> = Vec::with_capacity(req.columns.len() + 1);
        record.push(sample.clone());
        record.extend(req.rows[i].iter().cloned());
        wtr.write_record(&record)?;
    }
    wtr.flush()?;
    Ok(WriteMetadataReport {
        output_path: req.output_path.display().to_string(),
        sample_count: req.sample_names.len(),
        column_count: req.columns.len(),
    })
}

// ---------- helpers ----------

fn read_first_data_line<I: Iterator<Item = std::io::Result<String>>>(
    lines: &mut I,
) -> Result<String, FCError> {
    loop {
        match lines.next() {
            Some(Ok(line)) => {
                if line.starts_with('#') || line.trim().is_empty() {
                    continue;
                }
                return Ok(line);
            }
            Some(Err(e)) => return Err(FCError::Io(e)),
            None => return Err(FCError::NoHeader),
        }
    }
}

fn detect_delimiter(line: &str) -> char {
    if line.matches('\t').count() > line.matches(',').count() {
        '\t'
    } else {
        ','
    }
}

fn parse_columns(line: &str, delimiter: char) -> Vec<String> {
    line.split(delimiter)
        .map(|s| s.trim().trim_matches('"').to_string())
        .collect()
}

fn infer_sample_name(header: &str) -> String {
    let trimmed = header.trim();
    let base = if trimmed.contains('/') || trimmed.contains('\\') {
        Path::new(trimmed)
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or(trimmed)
    } else {
        trimmed
    };
    base.trim_end_matches(".bam")
        .trim_end_matches(".BAM")
        .trim_end_matches(".sam")
        .trim_end_matches(".SAM")
        .to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static NONCE: AtomicUsize = AtomicUsize::new(0);

    fn temp_file(name: &str, content: &str) -> PathBuf {
        let n = NONCE.fetch_add(1, Ordering::SeqCst);
        let dir = std::env::temp_dir().join(format!(
            "flashdeg_gui_fc_{}_{}_{}",
            std::process::id(),
            n,
            name
        ));
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("file.fc.txt");
        let mut f = std::fs::File::create(&path).unwrap();
        f.write_all(content.as_bytes()).unwrap();
        path
    }

    const FC_HEADER: &str = "# Program:featureCounts v2.0.3\n\
                             # Command: featureCounts -a annotation.gtf -o counts.txt sample.bam\n\
                             Geneid\tChr\tStart\tEnd\tStrand\tLength";

    #[test]
    fn inspects_one_sample_file() {
        let content = format!(
            "{header}\t/data/aligned/sample1.bam\n\
             ENSG001\t1\t100\t200\t+\t100\t42\n\
             ENSG002\t1\t300\t400\t-\t100\t77\n",
            header = FC_HEADER
        );
        let path = temp_file("one_sample", &content);
        let r = inspect(&path).unwrap();
        assert_eq!(r.geneid_column, "Geneid");
        assert_eq!(r.annotation_columns, vec!["Chr", "Start", "End", "Strand", "Length"]);
        assert_eq!(r.sample_columns.len(), 1);
        assert_eq!(r.sample_columns[0].inferred_sample_name, "sample1");
        assert_eq!(r.data_row_count, 2);
        assert_eq!(r.delimiter, "\\t");
    }

    #[test]
    fn inspects_multi_sample_file() {
        let content = format!(
            "{header}\tsample_a.bam\tsample_b.bam\tsample_c.bam\n\
             ENSG001\t1\t100\t200\t+\t100\t10\t20\t30\n\
             ENSG002\t1\t300\t400\t-\t100\t40\t50\t60\n",
            header = FC_HEADER
        );
        let path = temp_file("multi_sample", &content);
        let r = inspect(&path).unwrap();
        let names: Vec<_> = r.sample_columns.iter().map(|s| s.inferred_sample_name.clone()).collect();
        assert_eq!(names, vec!["sample_a", "sample_b", "sample_c"]);
    }

    #[test]
    fn rejects_missing_geneid() {
        let content = "Foo\tBar\nfoo\t1\n";
        let path = temp_file("no_geneid", content);
        let err = inspect(&path).unwrap_err();
        matches!(err, FCError::MissingGeneid);
    }

    #[test]
    fn merges_two_one_sample_files_with_intersection() {
        let s1 = format!(
            "{header}\tsample1.bam\nENSG001\t1\t100\t200\t+\t100\t10\nENSG002\t1\t300\t400\t-\t100\t20\n",
            header = FC_HEADER
        );
        let s2 = format!(
            "{header}\tsample2.bam\nENSG001\t1\t100\t200\t+\t100\t30\nENSG002\t1\t300\t400\t-\t100\t40\n",
            header = FC_HEADER
        );
        let p1 = temp_file("s1", &s1);
        let p2 = temp_file("s2", &s2);
        let out = std::env::temp_dir().join(format!(
            "flashdeg_gui_fc_merge_{}_{}.csv",
            std::process::id(),
            NONCE.fetch_add(1, Ordering::SeqCst)
        ));

        let r = merge_and_write(&MergeRequest {
            files: vec![
                FileSpec { path: p1, sample_overrides: BTreeMap::new() },
                FileSpec { path: p2, sample_overrides: BTreeMap::new() },
            ],
            output_counts_path: out.clone(),
            on_gene_set_diff: GeneSetDiffPolicy::Intersection,
        }).unwrap();
        assert_eq!(r.gene_count, 2);
        assert_eq!(r.sample_count, 2);
        assert_eq!(r.sample_names, vec!["sample1", "sample2"]);
        let written = std::fs::read_to_string(&out).unwrap();
        let lines: Vec<&str> = written.lines().collect();
        assert_eq!(lines[0], "gene_id,sample1,sample2");
        // BTreeSet keeps sorted order, so ENSG001 comes first.
        assert_eq!(lines[1], "ENSG001,10,30");
        assert_eq!(lines[2], "ENSG002,20,40");
        let _ = std::fs::remove_file(&out);
    }

    #[test]
    fn merge_intersection_drops_genes_not_in_both() {
        let s1 = format!(
            "{header}\tsample1.bam\nENSG001\t1\t100\t200\t+\t100\t10\nENSG002\t1\t300\t400\t-\t100\t20\nENSG003\t1\t500\t600\t+\t100\t30\n",
            header = FC_HEADER
        );
        let s2 = format!(
            "{header}\tsample2.bam\nENSG002\t1\t300\t400\t-\t100\t40\nENSG003\t1\t500\t600\t+\t100\t50\n",
            header = FC_HEADER
        );
        let p1 = temp_file("a", &s1);
        let p2 = temp_file("b", &s2);
        let out = std::env::temp_dir().join(format!(
            "flashdeg_gui_fc_merge2_{}_{}.csv",
            std::process::id(),
            NONCE.fetch_add(1, Ordering::SeqCst)
        ));

        let r = merge_and_write(&MergeRequest {
            files: vec![
                FileSpec { path: p1, sample_overrides: BTreeMap::new() },
                FileSpec { path: p2, sample_overrides: BTreeMap::new() },
            ],
            output_counts_path: out.clone(),
            on_gene_set_diff: GeneSetDiffPolicy::Intersection,
        }).unwrap();
        assert_eq!(r.gene_count, 2);
        assert!(r.warnings.iter().any(|w| w.contains("dropped 1 gene")));
        let _ = std::fs::remove_file(&out);
    }

    #[test]
    fn merge_error_policy_refuses_mismatch() {
        let s1 = format!("{header}\tsample1.bam\nENSG001\t1\t100\t200\t+\t100\t10\n", header = FC_HEADER);
        let s2 = format!("{header}\tsample2.bam\nENSG002\t1\t300\t400\t-\t100\t20\n", header = FC_HEADER);
        let p1 = temp_file("c", &s1);
        let p2 = temp_file("d", &s2);
        let out = std::env::temp_dir().join("nope.csv");
        let err = merge_and_write(&MergeRequest {
            files: vec![
                FileSpec { path: p1, sample_overrides: BTreeMap::new() },
                FileSpec { path: p2, sample_overrides: BTreeMap::new() },
            ],
            output_counts_path: out,
            on_gene_set_diff: GeneSetDiffPolicy::Error,
        }).unwrap_err();
        matches!(err, FCError::GeneSetMismatch);
    }

    #[test]
    fn merge_detects_duplicate_sample_names() {
        let s1 = format!("{header}\tsample.bam\nENSG001\t1\t100\t200\t+\t100\t10\n", header = FC_HEADER);
        let s2 = format!("{header}\tsample.bam\nENSG001\t1\t100\t200\t+\t100\t20\n", header = FC_HEADER);
        let p1 = temp_file("e", &s1);
        let p2 = temp_file("f", &s2);
        let out = std::env::temp_dir().join("dup.csv");
        let err = merge_and_write(&MergeRequest {
            files: vec![
                FileSpec { path: p1, sample_overrides: BTreeMap::new() },
                FileSpec { path: p2, sample_overrides: BTreeMap::new() },
            ],
            output_counts_path: out,
            on_gene_set_diff: GeneSetDiffPolicy::Intersection,
        }).unwrap_err();
        match err {
            FCError::DuplicateSample(s) => assert_eq!(s, "sample"),
            other => panic!("wrong error: {other:?}"),
        }
    }

    #[test]
    fn sample_override_replaces_inferred_name() {
        let content = format!(
            "{header}\tsample_x.bam\nENSG001\t1\t100\t200\t+\t100\t10\n",
            header = FC_HEADER
        );
        let p = temp_file("ovr", &content);
        let out = std::env::temp_dir().join(format!(
            "flashdeg_gui_fc_ovr_{}_{}.csv",
            std::process::id(),
            NONCE.fetch_add(1, Ordering::SeqCst)
        ));
        let mut overrides = BTreeMap::new();
        overrides.insert("sample_x.bam".to_string(), "renamed_sample".to_string());
        let r = merge_and_write(&MergeRequest {
            files: vec![FileSpec { path: p, sample_overrides: overrides }],
            output_counts_path: out.clone(),
            on_gene_set_diff: GeneSetDiffPolicy::Intersection,
        }).unwrap();
        assert_eq!(r.sample_names, vec!["renamed_sample"]);
        let written = std::fs::read_to_string(&out).unwrap();
        assert!(written.starts_with("gene_id,renamed_sample\n"));
        let _ = std::fs::remove_file(&out);
    }

    #[test]
    fn merge_escapes_sample_names_with_commas() {
        let content = format!(
            "{header}\tsample_x.bam\nENSG001\t1\t100\t200\t+\t100\t10\n",
            header = FC_HEADER
        );
        let p = temp_file("comma", &content);
        let out = std::env::temp_dir().join(format!(
            "flashdeg_gui_fc_comma_{}_{}.csv",
            std::process::id(),
            NONCE.fetch_add(1, Ordering::SeqCst)
        ));
        let mut overrides = BTreeMap::new();
        overrides.insert("sample_x.bam".to_string(), "weird,name".to_string());
        merge_and_write(&MergeRequest {
            files: vec![FileSpec { path: p, sample_overrides: overrides }],
            output_counts_path: out.clone(),
            on_gene_set_diff: GeneSetDiffPolicy::Intersection,
        })
        .unwrap();
        let written = std::fs::read_to_string(&out).unwrap();
        // The comma-bearing header must be quoted, not split into two columns.
        assert!(written.contains("\"weird,name\""), "got: {written}");
        // Re-parse with the csv crate to confirm it round-trips to 2 columns.
        let mut rdr = csv::Reader::from_path(&out).unwrap();
        let headers = rdr.headers().unwrap();
        assert_eq!(headers.len(), 2);
        assert_eq!(&headers[1], "weird,name");
        let _ = std::fs::remove_file(&out);
    }

    #[test]
    fn writes_metadata_csv() {
        let out = std::env::temp_dir().join(format!(
            "flashdeg_gui_fc_meta_{}_{}.csv",
            std::process::id(),
            NONCE.fetch_add(1, Ordering::SeqCst)
        ));
        let r = write_metadata(&WriteMetadataRequest {
            output_path: out.clone(),
            sample_id_column: "sample_id".to_string(),
            sample_names: vec!["s1".into(), "s2".into(), "s3".into()],
            columns: vec!["condition".into(), "batch".into()],
            rows: vec![
                vec!["control".into(), "A".into()],
                vec!["treated".into(), "A".into()],
                vec!["treated".into(), "B".into()],
            ],
        }).unwrap();
        assert_eq!(r.sample_count, 3);
        assert_eq!(r.column_count, 2);
        let written = std::fs::read_to_string(&out).unwrap();
        let lines: Vec<&str> = written.lines().collect();
        assert_eq!(lines[0], "sample_id,condition,batch");
        assert_eq!(lines[1], "s1,control,A");
        assert_eq!(lines[2], "s2,treated,A");
        assert_eq!(lines[3], "s3,treated,B");
        let _ = std::fs::remove_file(&out);
    }
}
