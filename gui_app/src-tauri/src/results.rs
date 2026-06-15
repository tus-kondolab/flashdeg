// DEG results CSV parser. See gui_plan.md § 5.3 and § 8.
//
// Required columns (FlashDEG / DESeq2 R / PyDESeq2 / InMoose are all
// compatible):
//   gene_id, baseMean, log2FoldChange, lfcSE, stat, pvalue, padj
//
// Optional columns:
//   tool         — provenance (e.g. "flashdeg", "deseq2-r")
//   adj_pvalue   — InMoose's name for `padj`. Treated as an alias.
//
// Missing-value tokens accepted: empty, NA, NaN, nan, null.

use std::fs::File;
use std::io::BufReader;
use std::path::Path;

use serde::{Deserialize, Serialize};

pub const REQUIRED_COLUMNS: &[&str] = &[
    "gene_id",
    "baseMean",
    "log2FoldChange",
    "lfcSE",
    "stat",
    "pvalue",
    "padj",
];

/// One row of DEG results. Field names match the canonical FlashDEG CSV
/// columns so the JSON wire format is consumable by the frontend without
/// translation.
/// One row of DEG results. Only `gene_id` is required; every numeric field
/// is optional because DESeq2 R writes `NA` for log2FoldChange / lfcSE /
/// stat / pvalue / padj on Cook-outlier replaced rows, all-zero genes, and
/// independent-filtered rows.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[allow(non_snake_case)]
pub struct ResultsRow {
    pub gene_id: String,
    pub baseMean: Option<f64>,
    pub log2FoldChange: Option<f64>,
    pub lfcSE: Option<f64>,
    pub stat: Option<f64>,
    pub pvalue: Option<f64>,
    pub padj: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none", default)]
    pub tool: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ResultsTable {
    pub source_path: String,
    pub source_tool: Option<String>,
    pub rows: Vec<ResultsRow>,
    pub n_total: usize,
    pub n_with_padj: usize,
}

#[derive(Debug, thiserror::Error)]
pub enum ResultsError {
    #[error("I/O error reading {path}: {source}")]
    Io {
        path: String,
        #[source]
        source: std::io::Error,
    },
    #[error("missing required column: {0}")]
    MissingColumn(&'static str),
    #[error("CSV format error: {0}")]
    Csv(#[from] csv::Error),
    #[error("row {row}, column {column}: cannot parse {value:?} as a number")]
    BadField {
        row: usize,
        column: &'static str,
        value: String,
    },
    #[error("row {row}, column gene_id: empty value")]
    EmptyGeneId { row: usize },
}

pub fn load_csv(path: &Path) -> Result<ResultsTable, ResultsError> {
    let file = File::open(path).map_err(|source| ResultsError::Io {
        path: path.display().to_string(),
        source,
    })?;
    let mut reader = csv::ReaderBuilder::new()
        .flexible(true)
        .from_reader(BufReader::new(file));

    let headers = reader.headers()?.clone();
    let header_index = HeaderIndex::resolve(&headers)?;

    let mut rows: Vec<ResultsRow> = Vec::new();
    let mut n_with_padj = 0usize;
    let mut tool_set: Option<String> = None;

    for (i, record) in reader.records().enumerate() {
        let record = record?;
        let row = parse_row(&record, &header_index, i + 2)?;
        if row.padj.is_some() {
            n_with_padj += 1;
        }
        if let Some(t) = &row.tool {
            tool_set.get_or_insert_with(|| t.clone());
        }
        rows.push(row);
    }

    Ok(ResultsTable {
        source_path: path.display().to_string(),
        source_tool: tool_set,
        n_total: rows.len(),
        n_with_padj,
        rows,
    })
}

#[derive(Debug)]
struct HeaderIndex {
    gene_id: usize,
    base_mean: usize,
    log2_fold_change: usize,
    lfc_se: usize,
    stat: usize,
    pvalue: usize,
    padj: usize,
    tool: Option<usize>,
}

impl HeaderIndex {
    fn resolve(headers: &csv::StringRecord) -> Result<Self, ResultsError> {
        let find = |name: &'static str| -> Result<usize, ResultsError> {
            headers
                .iter()
                .position(|h| h == name)
                .ok_or(ResultsError::MissingColumn(name))
        };
        Ok(Self {
            gene_id: find("gene_id")?,
            base_mean: find("baseMean")?,
            log2_fold_change: find("log2FoldChange")?,
            lfc_se: find("lfcSE")?,
            stat: find("stat")?,
            pvalue: find("pvalue")?,
            padj: headers
                .iter()
                .position(|h| h == "padj" || h == "adj_pvalue")
                .ok_or(ResultsError::MissingColumn("padj"))?,
            tool: headers.iter().position(|h| h == "tool"),
        })
    }
}

#[allow(non_snake_case)]
fn parse_row(
    record: &csv::StringRecord,
    idx: &HeaderIndex,
    row_number: usize,
) -> Result<ResultsRow, ResultsError> {
    let gene_id = record
        .get(idx.gene_id)
        .unwrap_or("")
        .trim()
        .to_string();
    if gene_id.is_empty() {
        return Err(ResultsError::EmptyGeneId { row: row_number });
    }

    let baseMean = parse_numeric(record, idx.base_mean, "baseMean", row_number)?;
    let log2FoldChange =
        parse_numeric(record, idx.log2_fold_change, "log2FoldChange", row_number)?;
    let lfcSE = parse_numeric(record, idx.lfc_se, "lfcSE", row_number)?;
    let stat = parse_numeric(record, idx.stat, "stat", row_number)?;
    let pvalue = parse_numeric(record, idx.pvalue, "pvalue", row_number)?;
    let padj = parse_numeric(record, idx.padj, "padj", row_number)?;
    let tool = idx
        .tool
        .and_then(|i| record.get(i))
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty());

    Ok(ResultsRow {
        gene_id,
        baseMean,
        log2FoldChange,
        lfcSE,
        stat,
        pvalue,
        padj,
        tool,
    })
}

/// Parses a numeric cell. Empty / NA / NaN / null / "." are reported as
/// `None`. Garbage (e.g. `"foo"` in a numeric column) is reported as
/// `BadField`. Numeric infinities are also coerced to `None` so the
/// frontend never has to special-case them.
fn parse_numeric(
    record: &csv::StringRecord,
    idx: usize,
    column: &'static str,
    row: usize,
) -> Result<Option<f64>, ResultsError> {
    let raw = record.get(idx).unwrap_or("").trim();
    if is_missing(raw) {
        return Ok(None);
    }
    match raw.parse::<f64>() {
        Ok(v) if v.is_finite() => Ok(Some(v)),
        Ok(_) => Ok(None),
        Err(_) => Err(ResultsError::BadField {
            row,
            column,
            value: raw.to_string(),
        }),
    }
}

fn is_missing(s: &str) -> bool {
    s.is_empty()
        || s.eq_ignore_ascii_case("na")
        || s.eq_ignore_ascii_case("nan")
        || s.eq_ignore_ascii_case("null")
        || s == "."
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn temp_csv(name: &str, content: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("flashdeg_gui_test_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join(name);
        let mut f = std::fs::File::create(&path).unwrap();
        f.write_all(content.as_bytes()).unwrap();
        path
    }

    #[test]
    fn parses_flashdeg_canonical_schema() {
        let path = temp_csv(
            "ok.csv",
            "gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj\n\
             g1,123.4,1.5,0.2,7.5,1e-12,2e-10\n\
             g2,50.0,-2.0,0.4,-5.0,1e-6,3e-5\n",
        );
        let table = load_csv(&path).unwrap();
        assert_eq!(table.n_total, 2);
        assert_eq!(table.n_with_padj, 2);
        assert_eq!(table.rows[0].gene_id, "g1");
        assert!((table.rows[0].log2FoldChange.unwrap() - 1.5).abs() < 1e-12);
    }

    #[test]
    fn accepts_inmoose_adj_pvalue_alias() {
        let path = temp_csv(
            "inmoose.csv",
            "gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,adj_pvalue\n\
             g1,100,1.0,0.3,3.3,0.001,0.01\n",
        );
        let table = load_csv(&path).unwrap();
        assert_eq!(table.rows[0].padj, Some(0.01));
    }

    #[test]
    fn treats_na_as_missing_optional() {
        let path = temp_csv(
            "na.csv",
            "gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj\n\
             g1,10.0,1.0,NA,NA,NA,NA\n",
        );
        let table = load_csv(&path).unwrap();
        assert!(table.rows[0].padj.is_none());
        assert!(table.rows[0].pvalue.is_none());
        assert_eq!(table.n_with_padj, 0);
    }

    #[test]
    fn accepts_na_on_log2_fold_change() {
        // DESeq2 R writes NA for log2FoldChange on Cook-replaced rows and
        // all-zero genes. This must not be a parse error.
        let path = temp_csv(
            "filtered.csv",
            "gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj\n\
             g1,0,NA,NA,NA,NA,NA\n\
             g2,5.3,NA,NA,NA,NA,NA\n\
             g3,50.0,1.5,0.3,5.0,1e-7,2e-6\n",
        );
        let table = load_csv(&path).unwrap();
        assert_eq!(table.n_total, 3);
        assert!(table.rows[0].log2FoldChange.is_none());
        assert!(table.rows[1].log2FoldChange.is_none());
        assert_eq!(table.rows[2].log2FoldChange, Some(1.5));
    }

    #[test]
    fn rejects_garbage_in_numeric_column() {
        let path = temp_csv(
            "bad_value.csv",
            "gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj\n\
             g1,10.0,not_a_number,0.3,3.3,0.001,0.01\n",
        );
        let err = load_csv(&path).unwrap_err();
        match err {
            ResultsError::BadField { column, value, .. } => {
                assert_eq!(column, "log2FoldChange");
                assert_eq!(value, "not_a_number");
            }
            other => panic!("wrong error: {other:?}"),
        }
    }

    #[test]
    fn rejects_missing_required_column() {
        let path = temp_csv(
            "bad.csv",
            "gene_id,baseMean,log2FoldChange,stat,pvalue,padj\ng1,1,1,1,1,1\n",
        );
        let err = load_csv(&path).unwrap_err();
        match err {
            ResultsError::MissingColumn(c) => assert_eq!(c, "lfcSE"),
            other => panic!("wrong error: {other:?}"),
        }
    }

    #[test]
    fn rejects_empty_gene_id() {
        let path = temp_csv(
            "empty.csv",
            "gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj\n,1,1,1,1,1,1\n",
        );
        let err = load_csv(&path).unwrap_err();
        match err {
            ResultsError::EmptyGeneId { row } => assert_eq!(row, 2),
            other => panic!("wrong error: {other:?}"),
        }
    }

    #[test]
    fn captures_tool_column_when_present() {
        let path = temp_csv(
            "tool.csv",
            "gene_id,baseMean,log2FoldChange,lfcSE,stat,pvalue,padj,tool\n\
             g1,10,1,0.3,3.3,0.001,0.01,deseq2-r\n",
        );
        let table = load_csv(&path).unwrap();
        assert_eq!(table.source_tool.as_deref(), Some("deseq2-r"));
    }
}
