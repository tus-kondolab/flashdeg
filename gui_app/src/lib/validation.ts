// Validation types. Mirrors src-tauri/src/validation.rs.

import type { Contrast } from "./project";

export type Severity = "block" | "warn";

export interface Finding {
  severity: Severity;
  code: string;
  category: string;
  message: string;
  suggested_fix?: string;
  context?: string;
}

export interface ValidationRequest {
  counts_path: string;
  metadata_path: string;
  design_formula: string | null;
  contrast: Contrast;
  ref_levels: Record<string, string>;
  condition_column: string;
  experiment_group: string | null;
  control_group: string | null;
  project_dir: string | null;
  /** Run design/contrast checks. Data step passes false (contrast not set yet). */
  include_design_checks: boolean;
}

export interface CountsSummary {
  gene_count: number;
  sample_count: number;
  gene_id_column: string;
  sample_columns: string[];
  delimiter: string;
}

export interface MetadataSummary {
  sample_count: number;
  sample_id_column: string;
  columns: string[];
  column_value_distributions: Record<string, Record<string, number>>;
}

export interface ValidationResult {
  findings: Finding[];
  counts_summary?: CountsSummary;
  metadata_summary?: MetadataSummary;
}

export function groupFindings(findings: Finding[]): {
  blockers: Finding[];
  warnings: Finding[];
} {
  const blockers: Finding[] = [];
  const warnings: Finding[] = [];
  for (const f of findings) {
    if (f.severity === "block") blockers.push(f);
    else warnings.push(f);
  }
  return { blockers, warnings };
}
