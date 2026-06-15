// Shared row→figure projection for both plot panels, used by the interactive
// view (Volcano/MAPlot) and the export preview so they draw identical data.

import type { ResultsRow } from "../../lib/results";
import { VOLCANO_COLORS } from "../../lib/colors";
import { projectRows, type LabelPoint } from "./types";
import type { LineShapeLite, Series } from "./plot_core";

export type PanelId = "volcano" | "ma";

export interface PanelFigure {
  series: Series[];
  xTitle: string;
  yTitle: string;
  shapes: LineShapeLite[];
  hoverFormat: (gene: string, x: number, y: number) => string;
}

const CUTOFF = { color: "rgba(127,127,127,0.4)", dash: "dot", width: 1 };
const NS_R = 1.5;
const SIG_R = 2;
const negLog10Padj = (r: ResultsRow) =>
  r.padj == null ? null : -Math.log10(Math.max(r.padj, Number.MIN_VALUE));

export function buildPanelFigure(
  panel: PanelId,
  rows: ResultsRow[],
  padjCutoff: number,
  log2fcCutoff: number,
): PanelFigure {
  if (panel === "volcano") {
    const c = projectRows(rows, padjCutoff, log2fcCutoff, (r) => r.log2FoldChange, negLog10Padj);
    const yCutoff = -Math.log10(padjCutoff);
    return {
      series: [
        { name: "NS", color: VOLCANO_COLORS.ns, radius: NS_R, ...c.ns },
        { name: "Down", color: VOLCANO_COLORS.down, radius: SIG_R, ...c.down },
        { name: "Up", color: VOLCANO_COLORS.up, radius: SIG_R, ...c.up },
      ],
      xTitle: "log2FoldChange",
      yTitle: "-log10(padj)",
      shapes: [
        { type: "line", xref: "x", yref: "y", x0: -log2fcCutoff, x1: -log2fcCutoff, y0: 0, y1: 1, line: CUTOFF },
        { type: "line", xref: "x", yref: "y", x0: log2fcCutoff, x1: log2fcCutoff, y0: 0, y1: 1, line: CUTOFF },
        { type: "line", xref: "paper", yref: "y", x0: 0, x1: 1, y0: yCutoff, y1: yCutoff, line: CUTOFF },
      ],
      hoverFormat: (g, x, y) => `${g}\nlog2FC = ${x.toFixed(3)}\n-log10(padj) = ${y.toFixed(2)}`,
    };
  }
  const c = projectRows(
    rows,
    padjCutoff,
    log2fcCutoff,
    (r) => (r.baseMean != null && r.baseMean > 0 ? Math.log10(r.baseMean) : null),
    (r) => r.log2FoldChange,
  );
  return {
    series: [
      { name: "NS", color: VOLCANO_COLORS.ns, radius: NS_R, ...c.ns },
      { name: "Down", color: VOLCANO_COLORS.down, radius: SIG_R, ...c.down },
      { name: "Up", color: VOLCANO_COLORS.up, radius: SIG_R, ...c.up },
    ],
    xTitle: "log10(baseMean)",
    yTitle: "log2FoldChange",
    shapes: [{ type: "line", xref: "paper", yref: "y", x0: 0, x1: 1, y0: 0, y1: 0, line: { color: "rgba(127,127,127,0.4)", width: 1 } }],
    hoverFormat: (g, x, y) => `${g}\nlog10(baseMean) = ${x.toFixed(2)}\nlog2FC = ${y.toFixed(3)}`,
  };
}

export function panelLabelPoints(
  panel: PanelId,
  rows: ResultsRow[],
  labeled: Set<string>,
  displayName?: (geneId: string) => string,
): LabelPoint[] {
  if (labeled.size === 0) return [];
  const out: LabelPoint[] = [];
  for (const r of rows) {
    if (!labeled.has(r.gene_id)) continue;
    // Identity stays gene_id; `label` is the rendered text (symbol when mapped).
    const label = displayName ? displayName(r.gene_id) : undefined;
    if (panel === "volcano") {
      if (r.log2FoldChange == null || r.padj == null) continue;
      out.push({ x: r.log2FoldChange, y: -Math.log10(Math.max(r.padj, Number.MIN_VALUE)), gene: r.gene_id, label });
    } else {
      if (r.baseMean == null || r.baseMean <= 0 || r.log2FoldChange == null) continue;
      out.push({ x: Math.log10(r.baseMean), y: r.log2FoldChange, gene: r.gene_id, label });
    }
  }
  return out;
}
