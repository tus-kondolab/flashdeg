// Basic sortable, filterable results table. See gui_plan.md § 8.1.
//
// PoC-level: renders up to MAX_RENDERED_ROWS rows at a time. For large
// result sets the user's current sort + filter narrows down what is shown.
// TODO: replace with react-window virtualization once filter set typically
// stays under a few hundred but full sort needs full traversal.

import { useEffect, useMemo, useRef, useState } from "react";
import type { ResultsRow } from "../lib/results";
import type { GeneNameMode } from "../lib/gene_symbols";

const PAGE_SIZE = 1000;

type SortKey =
  | "gene_id"
  | "baseMean"
  | "log2FoldChange"
  | "lfcSE"
  | "stat"
  | "pvalue"
  | "padj";

export interface ResultsTableProps {
  rows: ResultsRow[];
  sortKey: SortKey;
  sortDir: "asc" | "desc";
  onSort: (key: SortKey) => void;
  onHover?: (gene: string | null) => void;
  /** Genes checked for on-plot labeling. */
  labeledGenes: Set<string>;
  onToggleLabel: (gene: string) => void;
  /** ENSG vs symbol display. Affects only the gene column's rendered text. */
  geneNameMode?: GeneNameMode;
  /** Map a gene_id to its displayed name (gene symbol when in symbol mode). */
  displayName?: (geneId: string) => string;
  /** Gene selected on the plot: the table pages to it, scrolls it into view,
   *  and highlights its row. */
  selectedGene?: string | null;
}

const COLUMNS: { key: SortKey; label: string; align: "left" | "right" }[] = [
  { key: "gene_id", label: "gene_id", align: "left" },
  { key: "baseMean", label: "baseMean", align: "right" },
  { key: "log2FoldChange", label: "log2FC", align: "right" },
  { key: "lfcSE", label: "lfcSE", align: "right" },
  { key: "stat", label: "stat", align: "right" },
  { key: "pvalue", label: "pvalue", align: "right" },
  { key: "padj", label: "padj", align: "right" },
];

export function ResultsTable({
  rows,
  sortKey,
  sortDir,
  onSort,
  onHover,
  labeledGenes,
  onToggleLabel,
  geneNameMode = "ensembl",
  displayName,
  selectedGene,
}: ResultsTableProps) {
  const sorted = useMemo(() => sortRows(rows, sortKey, sortDir), [rows, sortKey, sortDir]);
  const nameOf = (id: string) => (displayName ? displayName(id) : id);

  // Page through all rows (1000 at a time) instead of truncating. Reset to the
  // first page whenever the row set or sort changes (e.g. a new search).
  const [page, setPage] = useState(0);
  useEffect(() => { setPage(0); }, [rows, sortKey, sortDir]);

  // Jump to the page holding a newly selected gene (read sorted via ref so this
  // fires only on selection change, not on every sort/filter change).
  const sortedRef = useRef(sorted);
  sortedRef.current = sorted;
  useEffect(() => {
    if (!selectedGene) return;
    const idx = sortedRef.current.findIndex((r) => r.gene_id === selectedGene);
    if (idx >= 0) setPage(Math.floor(idx / PAGE_SIZE));
  }, [selectedGene]);

  const pageCount = Math.max(1, Math.ceil(sorted.length / PAGE_SIZE));
  const safePage = Math.min(page, pageCount - 1);
  const start = safePage * PAGE_SIZE;
  const visible = sorted.slice(start, start + PAGE_SIZE);

  // Scroll the selected row into view once it's on the current page. Center it
  // ("nearest" would tuck a top-aligned row under the sticky header, making it
  // look a few rows off / hidden).
  const selectedRowRef = useRef<HTMLTableRowElement | null>(null);
  useEffect(() => {
    if (selectedGene && selectedRowRef.current) {
      selectedRowRef.current.scrollIntoView({ block: "center" });
    }
  }, [selectedGene, safePage]);

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%", minHeight: 0 }}>
      <div style={{ flex: 1, minHeight: 0, overflow: "auto" }}>
        <table style={{ width: "100%", borderCollapse: "collapse", fontSize: 12 }}>
          <thead>
            <tr style={{ position: "sticky", top: 0, background: "var(--table-head, #f3f3f3)" }}>
              <th
                style={{
                  padding: "6px 6px",
                  borderBottom: "1px solid rgba(127,127,127,0.3)",
                  fontWeight: 500,
                }}
                title="Label this gene on the plot"
              >
                🏷
              </th>
              {COLUMNS.map((c) => {
                const isActive = c.key === sortKey;
                return (
                  <th
                    key={c.key}
                    onClick={() => onSort(c.key)}
                    style={{
                      padding: "6px 8px",
                      textAlign: c.align,
                      cursor: "pointer",
                      borderBottom: "1px solid rgba(127,127,127,0.3)",
                      userSelect: "none",
                      fontWeight: isActive ? 700 : 500,
                    }}
                  >
                    {c.key === "gene_id" && geneNameMode === "symbol" ? "symbol" : c.label}
                    {isActive && (sortDir === "asc" ? " ▲" : " ▼")}
                  </th>
                );
              })}
            </tr>
          </thead>
          <tbody>
            {visible.map((r) => {
              const isSelected = r.gene_id === selectedGene;
              return (
                <tr
                  key={r.gene_id}
                  ref={isSelected ? selectedRowRef : undefined}
                  onMouseEnter={() => onHover?.(r.gene_id)}
                  onMouseLeave={() => onHover?.(null)}
                  style={{ background: isSelected ? "rgba(0,114,178,0.12)" : "transparent" }}
                >
                  <td
                    style={{ ...cellStyle("left"), textAlign: "center", cursor: "pointer" }}
                    onClick={() => onToggleLabel(r.gene_id)}
                  >
                    <input
                      type="checkbox"
                      checked={labeledGenes.has(r.gene_id)}
                      onChange={() => onToggleLabel(r.gene_id)}
                      onClick={(e) => e.stopPropagation()}
                    />
                  </td>
                  <td style={cellStyle("left")} title={geneNameMode === "symbol" ? r.gene_id : undefined}>{nameOf(r.gene_id)}</td>
                  <td style={cellStyle("right")}>{fmtNumOrNull(r.baseMean, 2)}</td>
                  <td style={cellStyle("right")}>{fmtNumOrNull(r.log2FoldChange, 3)}</td>
                  <td style={cellStyle("right")}>{fmtNumOrNull(r.lfcSE, 3)}</td>
                  <td style={cellStyle("right")}>{fmtNumOrNull(r.stat, 3)}</td>
                  <td style={cellStyle("right")}>{fmtPvalueOrNull(r.pvalue)}</td>
                  <td style={cellStyle("right")}>{fmtPvalueOrNull(r.padj)}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: 8, padding: "5px 8px", fontSize: 11, borderTop: "1px solid rgba(127,127,127,0.2)" }}>
        <span style={{ opacity: 0.7 }}>
          {sorted.length === 0
            ? "No rows"
            : `${(start + 1).toLocaleString()}–${(start + visible.length).toLocaleString()} of ${sorted.length.toLocaleString()}`}
        </span>
        {pageCount > 1 && (
          <span style={{ display: "flex", alignItems: "center", gap: 4, marginLeft: "auto" }}>
            <button onClick={() => setPage(0)} disabled={safePage === 0} style={pageBtn} title="First page">⏮ First</button>
            <button onClick={() => setPage((p) => Math.max(0, p - 1))} disabled={safePage === 0} style={pageBtn} title="Previous page">‹ Prev</button>
            <span style={{ opacity: 0.7, padding: "0 4px" }}>Page {safePage + 1} / {pageCount}</span>
            <button onClick={() => setPage((p) => Math.min(pageCount - 1, p + 1))} disabled={safePage >= pageCount - 1} style={pageBtn} title="Next page">Next ›</button>
            <button onClick={() => setPage(pageCount - 1)} disabled={safePage >= pageCount - 1} style={pageBtn} title="Last page">Last ⏭</button>
          </span>
        )}
      </div>
    </div>
  );
}

const pageBtn: React.CSSProperties = { fontSize: 11, padding: "1px 8px", cursor: "pointer" };

function cellStyle(align: "left" | "right"): React.CSSProperties {
  return {
    padding: "4px 8px",
    textAlign: align,
    borderBottom: "1px solid rgba(127,127,127,0.1)",
    whiteSpace: "nowrap",
    fontFamily:
      "var(--mono)",
  };
}

function fmtNum(v: number, digits: number): string {
  if (!Number.isFinite(v)) return "—";
  return v.toFixed(digits);
}

function fmtNumOrNull(v: number | null, digits: number): string {
  if (v == null) return "NA";
  return fmtNum(v, digits);
}

function fmtPvalueOrNull(v: number | null): string {
  if (v == null) return "NA";
  if (!Number.isFinite(v)) return "—";
  if (v === 0) return "0";
  if (v < 1e-4 || v >= 1e4) return v.toExponential(2);
  return v.toPrecision(3);
}

function sortRows(rows: ResultsRow[], key: SortKey, dir: "asc" | "desc"): ResultsRow[] {
  const sign = dir === "asc" ? 1 : -1;
  const arr = rows.slice();
  if (key === "gene_id") {
    arr.sort((a, b) => sign * a.gene_id.localeCompare(b.gene_id));
    return arr;
  }
  arr.sort((a, b) => {
    const va = a[key];
    const vb = b[key];
    // null/NaN goes to the end regardless of direction.
    const ra = va == null || Number.isNaN(va) ? Infinity : va;
    const rb = vb == null || Number.isNaN(vb) ? Infinity : vb;
    if (ra === Infinity && rb === Infinity) return 0;
    if (ra === Infinity) return 1;
    if (rb === Infinity) return -1;
    return sign * (ra < rb ? -1 : ra > rb ? 1 : 0);
  });
  return arr;
}

export type { SortKey };
