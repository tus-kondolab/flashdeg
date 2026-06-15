// DEG results schema. Mirrors src-tauri/src/results.rs.
//
// Field names match the canonical FlashDEG CSV columns exactly so JSON IPC
// requires no translation. NA / NaN values from the CSV are reported as
// JSON null on the optional fields.

export interface ResultsRow {
  gene_id: string;
  baseMean: number | null;
  log2FoldChange: number | null;
  lfcSE: number | null;
  stat: number | null;
  pvalue: number | null;
  padj: number | null;
  tool?: string;
}

export interface ResultsTable {
  source_path: string;
  source_tool: string | null;
  rows: ResultsRow[];
  n_total: number;
  n_with_padj: number;
}

export type SignificanceClass = "ns" | "up" | "down" | "filtered";

export function classifyRow(
  row: ResultsRow,
  padjCutoff: number,
  log2fcCutoff: number,
): SignificanceClass {
  if (row.padj == null || row.log2FoldChange == null) return "filtered";
  if (row.padj >= padjCutoff || Math.abs(row.log2FoldChange) < log2fcCutoff) {
    return "ns";
  }
  return row.log2FoldChange > 0 ? "up" : "down";
}

export interface SignificanceCounts {
  up: number;
  down: number;
  ns: number;
  filtered: number;
}

export function countSignificance(
  rows: ResultsRow[],
  padjCutoff: number,
  log2fcCutoff: number,
): SignificanceCounts {
  const counts: SignificanceCounts = { up: 0, down: 0, ns: 0, filtered: 0 };
  for (const r of rows) {
    counts[classifyRow(r, padjCutoff, log2fcCutoff)]++;
  }
  return counts;
}
