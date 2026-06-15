// featureCounts import types. Mirrors src-tauri/src/featurecounts.rs.

export interface SampleColumn {
  raw_header: string;
  inferred_sample_name: string;
}

export interface FeatureCountsInspection {
  path: string;
  /** "\\t" for tab or "," for comma. */
  delimiter: string;
  geneid_column: string;
  annotation_columns: string[];
  sample_columns: SampleColumn[];
  data_row_count: number;
}

export type GeneSetDiffPolicy = "error" | "intersection" | "union";

export interface FileSpec {
  path: string;
  sample_overrides: Record<string, string>;
}

export interface MergeRequest {
  files: FileSpec[];
  output_counts_path: string;
  on_gene_set_diff: GeneSetDiffPolicy;
}

export interface MergeReport {
  output_path: string;
  gene_count: number;
  sample_count: number;
  sample_names: string[];
  warnings: string[];
}

export interface WriteMetadataRequest {
  output_path: string;
  sample_id_column: string;
  sample_names: string[];
  columns: string[];
  rows: string[][];
}

export interface WriteMetadataReport {
  output_path: string;
  sample_count: number;
  column_count: number;
}
