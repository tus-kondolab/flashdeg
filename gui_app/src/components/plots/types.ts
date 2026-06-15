import type { ResultsRow, SignificanceClass } from "../../lib/results";
import { classifyRow } from "../../lib/results";

export type ExportFormat = "png" | "svg";

export interface ExportSize {
  /** viewBox / raster dimensions in pixels (= mm / 25.4 * dpi), so the
   *  embedded scatter raster is full resolution. */
  width: number;
  height: number;
  widthMm?: number; // x-axis LENGTH in mm (plot area, not the whole canvas)
  heightMm?: number; // y-axis length in mm
  dpi?: number;
  fontPt?: number; // base (axis) font size in points
  labelFontPt?: number; // gene-label font size in points
  pointRadiusMm?: number; // marker radius in mm (diameter px = 2r/25.4*dpi)
  axisWidthPt?: number; // axis line thickness in points
  xDtick?: number; // fixed x tick spacing (0/undefined = Plotly auto)
  yDtick?: number; // fixed y tick spacing
  xRange?: [number, number] | null; // explicit axis range (null = data extent)
  yRange?: [number, number] | null;
  manualOffsets?: Record<string, ManualOffset>; // manually-dragged label offsets
}

export const PLOT_FONT_FAMILY = "Arial, Helvetica, sans-serif";

/** Readable base font for the interactive on-screen plots (px). The
 *  publication point size only affects export + the preview image. */
export const SCREEN_FONT = 13;

// Returns the rendered image bytes at the requested pixel size; the caller
// decides where to save them. See gui_plan.md § 12.
export type ExportFn = (format: ExportFormat, size?: ExportSize) => Promise<Uint8Array>;

/** A manually-placed label offset, stored as a DATA-space delta from the
 *  anchor to the label centre, so it maps identically across renders
 *  regardless of pixel size / aspect (preview vs export). */
export interface ManualOffset {
  dx: number;
  dy: number;
}

/** Publication-export options shared by the plots. */
export interface PubOptions {
  fontSize: number;
  xRange: [number, number] | null;
  yRange: [number, number] | null;
  /** Genes to label on the plot (with leader lines). */
  labeledGenes: Set<string>;
  /** Per-gene manual label offsets (set by dragging in the export preview);
   *  override the automatic repel placement for those genes in the export. */
  manualOffsets?: Record<string, ManualOffset>;
  /** Bumped by the Preview button to snap the on-screen view to the spec
   *  (overrides any manual zoom via Plotly uirevision). */
  revision: number;
}

export interface CommonPlotProps {
  rows: ResultsRow[];
  padjCutoff: number;
  log2fcCutoff: number;
  pub?: PubOptions;
  /** Map a gene_id to the text shown on its label (e.g. gene symbol). Identity
   *  stays the gene_id; only the rendered label text changes. */
  displayName?: (geneId: string) => string;
  onHover?: (gene: string | null) => void;
  /** Toggle a gene's on-plot label (e.g. on dot click). */
  onToggleLabel?: (gene: string) => void;
  /** Currently selected gene (its label/anchor is highlighted). */
  selectedGene?: string | null;
  /** Select a gene (e.g. clicking its on-plot label), or null to clear. */
  onSelectGene?: (gene: string | null) => void;
  onRendered?: (elapsedMs: number) => void;
  // Called once the underlying chart instance is ready. Pass `null` on
  // unmount so the page can disable the export button.
  registerExport?: (fn: ExportFn | null) => void;
}

/** One on-plot gene label. */
export interface LabelPoint {
  x: number;
  y: number;
  gene: string; // identity (gene_id) — keys selection / manual offsets
  label?: string; // text to render instead of `gene` (e.g. gene symbol)
}

export interface PlotRect {
  left: number;
  top: number;
  width: number;
  height: number;
}

interface RepelItem {
  ax: number; // anchor pixel x
  ay: number; // anchor pixel y
  w: number; // label box width (px)
  h: number; // label box height (px)
  dir: number; // preferred horizontal side: +1 = right of dot, -1 = left
}

/**
 * Force-directed label placement (ggrepel-style), in PIXEL space. Labels
 * repel each other and stay off their own anchor, with a weak spring back to
 * the anchor to keep leader lines short. Deterministic (no randomness).
 * Returns the label-centre pixel position for each item.
 */
// Do two line segments (p1-p2) and (p3-p4) properly cross?
function segmentsCross(
  p1: { x: number; y: number }, p2: { x: number; y: number },
  p3: { x: number; y: number }, p4: { x: number; y: number },
): boolean {
  const cross = (a: { x: number; y: number }, b: { x: number; y: number }, c: { x: number; y: number }) =>
    (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  const d1 = cross(p3, p4, p1);
  const d2 = cross(p3, p4, p2);
  const d3 = cross(p1, p2, p3);
  const d4 = cross(p1, p2, p4);
  return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

function repelLabels(items: RepelItem[], bounds: { left: number; top: number; right: number; bottom: number }): { x: number; y: number }[] {
  const n = items.length;
  // Init: place each label just beside its dot on its preferred side
  // (data x >= 0 → right, x < 0 → left), vertically level with the dot. The
  // relaxation pass below then resolves any overlaps from this start.
  const pos = items.map((it) => ({
    x: it.ax + it.dir * (it.w / 2 + 6),
    y: it.ay,
  }));

  // One relaxation step: pairwise repulsion + per-label anchor constraints.
  // Returns true if any box was still overlapping (i.e. not yet settled).
  const step = (): boolean => {
    let moved = false;
    for (let i = 0; i < n; i++) {
      for (let j = i + 1; j < n; j++) {
        let dx = pos[j].x - pos[i].x;
        let dy = pos[j].y - pos[i].y;
        const ox = (items[i].w + items[j].w) / 2 + 2 - Math.abs(dx);
        const oy = (items[i].h + items[j].h) / 2 + 2 - Math.abs(dy);
        if (ox > 0 && oy > 0) {
          moved = true;
          let d = Math.hypot(dx, dy);
          if (d < 0.01) {
            dx = i % 2 === 0 ? 1 : -1;
            dy = -1;
            d = Math.hypot(dx, dy);
          }
          const push = Math.min(ox, oy) / 2;
          const ux = dx / d;
          const uy = dy / d;
          pos[i].x -= ux * push; pos[i].y -= uy * push;
          pos[j].x += ux * push; pos[j].y += uy * push;
        }
      }
    }
    for (let i = 0; i < n; i++) {
      let dx = pos[i].x - items[i].ax;
      let dy = pos[i].y - items[i].ay;
      // Keep the dot OUTSIDE the label box and beside it: when the anchor falls
      // within the box (vertically level AND horizontally overlapping), shove
      // the box sideways so its near edge clears the dot — toward whichever side
      // the label is already on, or its preferred side when it sits dead level.
      const padX = items[i].w / 2 + 4;
      const padY = items[i].h / 2 + 2;
      if (Math.abs(dx) < padX && Math.abs(dy) < padY) {
        const side = Math.abs(dx) < 1 ? items[i].dir : dx >= 0 ? 1 : -1;
        pos[i].x = items[i].ax + side * padX;
        dx = pos[i].x - items[i].ax;
        moved = true;
      }
      // Keep leaders short: pull back toward the anchor when too far (but never
      // closer than the horizontal box-clearance gap above).
      let dist = Math.hypot(dx, dy) || 1;
      const target = Math.max(items[i].h * 1.6 + items[i].w * 0.15, padX + 2);
      if (dist > target) {
        const pull = (dist - target) * 0.25;
        pos[i].x -= (dx / dist) * pull;
        pos[i].y -= (dy / dist) * pull;
      }
      pos[i].x = Math.min(bounds.right - items[i].w / 2, Math.max(bounds.left + items[i].w / 2, pos[i].x));
      pos[i].y = Math.min(bounds.bottom - items[i].h / 2, Math.max(bounds.top + items[i].h / 2, pos[i].y));
    }
    return moved;
  };

  for (let iter = 0; iter < 300; iter++) {
    if (!step() && iter > 5) break;
  }

  // Reduce leader-line crossings: when two leaders (anchor → label) cross,
  // swapping the two label positions uncrosses them. Re-settle briefly after
  // each round to absorb overlaps the swaps introduce.
  for (let round = 0; round < 5; round++) {
    let swapped = false;
    for (let i = 0; i < n; i++) {
      for (let j = i + 1; j < n; j++) {
        const ai = { x: items[i].ax, y: items[i].ay };
        const aj = { x: items[j].ax, y: items[j].ay };
        if (segmentsCross(ai, pos[i], aj, pos[j])) {
          const t = pos[i];
          pos[i] = pos[j];
          pos[j] = t;
          swapped = true;
        }
      }
    }
    if (!swapped) break;
    for (let k = 0; k < 40; k++) step();
  }
  return pos;
}

let measureCtx: CanvasRenderingContext2D | null = null;
function measureText(text: string, fontPx: number): number {
  if (!measureCtx) measureCtx = document.createElement("canvas").getContext("2d");
  if (!measureCtx) return text.length * fontPx * 0.6;
  measureCtx.font = `${fontPx}px ${PLOT_FONT_FAMILY}`;
  return measureCtx.measureText(text).width;
}

/** A placed gene label, in plot-rectangle PIXEL coordinates. */
export interface LabelPlacement {
  gene: string;
  label?: string; // text to render instead of `gene`
  x: number; // anchor data x
  y: number; // anchor data y
  anchor: { x: number; y: number }; // anchor pixel (the data point)
  center: { x: number; y: number }; // label-box centre pixel
  w: number;
  h: number;
}

/**
 * Place labeled genes with a force-directed (repel) layout in the given plot
 * rectangle / font size, returning PIXEL placements. Manually-placed genes use
 * their stored data-space offset; the rest are positioned by the solver.
 * Must be run separately per render (on-screen vs export differ in px/font).
 */
export function layoutLabels(
  labels: LabelPoint[],
  rect: PlotRect,
  xr: [number, number],
  yr: [number, number],
  fontPx: number,
  manual: Record<string, ManualOffset> = {},
): LabelPlacement[] {
  if (labels.length === 0) return [];
  const { left, top, width, height } = rect;
  const items: RepelItem[] = labels.map((l) => ({
    ax: left + ((l.x - xr[0]) / (xr[1] - xr[0])) * width,
    ay: top + (1 - (l.y - yr[0]) / (yr[1] - yr[0])) * height, // y is flipped
    // Tight box: no internal padding. Measure the rendered text (the symbol
    // when set, else the gene id).
    w: measureText(l.label ?? l.gene, fontPx),
    h: fontPx,
    // Preferred side from the data x sign (e.g. volcano: up-regulated → right).
    dir: l.x >= 0 ? 1 : -1,
  }));
  // Manually-placed labels: data-space delta → pixel centre. Others: repel.
  const center: ({ x: number; y: number } | null)[] = labels.map((l, i) => {
    const m = manual[l.gene];
    if (!m) return null;
    return {
      x: items[i].ax + (m.dx / (xr[1] - xr[0])) * width,
      y: items[i].ay - (m.dy / (yr[1] - yr[0])) * height, // y flipped
    };
  });
  const autoIdx = labels.map((_l, i) => i).filter((i) => center[i] === null);
  if (autoIdx.length > 0) {
    const autoCenters = repelLabels(
      autoIdx.map((i) => items[i]),
      { left, top, right: left + width, bottom: top + height },
    );
    autoIdx.forEach((i, k) => { center[i] = autoCenters[k]; });
  }
  return labels.map((l, i) => ({
    gene: l.gene,
    label: l.label,
    x: l.x,
    y: l.y,
    anchor: { x: items[i].ax, y: items[i].ay },
    center: center[i]!,
    w: items[i].w,
    h: items[i].h,
  }));
}

export interface ClassifiedSeries {
  ns: { x: number[]; y: number[]; ids: string[] };
  up: { x: number[]; y: number[]; ids: string[] };
  down: { x: number[]; y: number[]; ids: string[] };
}

/**
 * Project results to (x, y, gene_id) groups for plotting. `yProj` returns
 * the y value (e.g. -log10(padj) for volcano, log2FC for MA) or null when
 * the row should be skipped.
 */
export function projectRows(
  rows: ResultsRow[],
  padjCutoff: number,
  log2fcCutoff: number,
  xProj: (r: ResultsRow) => number | null,
  yProj: (r: ResultsRow) => number | null,
): ClassifiedSeries {
  const out: ClassifiedSeries = {
    ns: { x: [], y: [], ids: [] },
    up: { x: [], y: [], ids: [] },
    down: { x: [], y: [], ids: [] },
  };
  for (const r of rows) {
    const x = xProj(r);
    const y = yProj(r);
    if (x == null || y == null || !Number.isFinite(x) || !Number.isFinite(y)) {
      continue;
    }
    const klass: SignificanceClass = classifyRow(r, padjCutoff, log2fcCutoff);
    if (klass === "filtered") continue;
    out[klass].x.push(x);
    out[klass].y.push(y);
    out[klass].ids.push(r.gene_id);
  }
  return out;
}

