// Shared plotting core (no Plotly). Tick math + a single canvas drawing routine
// used by both the interactive view (ScatterCanvas) and the PNG export, so they
// stay pixel-consistent with each other and with the vector SVG export.

import { layoutLabels, PLOT_FONT_FAMILY, type LabelPlacement, type LabelPoint, type ManualOffset } from "./types";

export interface Series {
  name: string;
  color: string;
  radius: number; // marker radius in CSS px (interactive default)
  x: number[];
  y: number[];
  ids: string[];
}

export interface LineShapeLite {
  type?: string;
  xref?: string; // "x" (data) | "paper" (0..1)
  yref?: string; // "y" (data) | "paper" (0..1)
  x0?: number;
  x1?: number;
  y0?: number;
  y1?: number;
  line?: { color?: string; width?: number; dash?: string };
}

export interface PlotRectPx {
  left: number;
  top: number;
  width: number;
  height: number;
}

// --- "nice" ticks: 1-2-5 × 10ⁿ so labels land on round numbers ---------------

export function niceStep(range: number, target: number): number {
  const raw = range / Math.max(1, target);
  if (!(raw > 0)) return 1;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const nice = norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10;
  return nice * mag;
}

export function axisTicks(lo: number, hi: number, dtick?: number): { step: number; ticks: number[] } {
  const step = dtick && dtick > 0 ? dtick : niceStep(hi - lo, 5);
  const ticks: number[] = [];
  const start = Math.ceil(lo / step - 1e-9) * step;
  for (let v = start; v <= hi + step * 1e-9; v += step) {
    ticks.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  }
  return { step, ticks };
}

export function tickDecimals(step: number): number {
  if (step >= 1) return 0;
  return Math.min(6, Math.ceil(-Math.log10(step) - 1e-9));
}

export function fmtTick(v: number, decimals: number): string {
  const s = v.toFixed(decimals);
  return s === `-${(0).toFixed(decimals)}` ? (0).toFixed(decimals) : s;
}

/**
 * Leader-line endpoint on the label box: the nearest to the anchor (dot centre)
 * among the midpoints of the box's 4 edges.
 */
export function leaderPoint(
  ax: number, ay: number,
  cx: number, cy: number, w: number, h: number,
): { x: number; y: number } {
  const hx = w / 2;
  const hy = h / 2;
  const pts: [number, number][] = [
    [cx, cy - hy], [cx, cy + hy], [cx - hx, cy], [cx + hx, cy], // edge midpoints
  ];
  let best = pts[0];
  let bd = Infinity;
  for (const p of pts) {
    const d = (p[0] - ax) * (p[0] - ax) + (p[1] - ay) * (p[1] - ay);
    if (d < bd) { bd = d; best = p; }
  }
  return { x: best[0], y: best[1] };
}

// --- nearest-point hit index (uniform grid over the data extent) -------------

export interface HitIndex {
  minX: number;
  minY: number;
  cellW: number;
  cellH: number;
  cols: number;
  rows: number;
  cells: number[][]; // packed [seriesIdx, pointIdx, ...] per cell
}

export function buildHitIndex(series: Series[]): HitIndex {
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  let total = 0;
  for (const s of series) {
    const n = Math.min(s.x.length, s.y.length);
    total += n;
    for (let i = 0; i < n; i++) {
      if (s.x[i] < minX) minX = s.x[i];
      if (s.x[i] > maxX) maxX = s.x[i];
      if (s.y[i] < minY) minY = s.y[i];
      if (s.y[i] > maxY) maxY = s.y[i];
    }
  }
  if (!Number.isFinite(minX)) { minX = 0; maxX = 1; minY = 0; maxY = 1; }
  const side = Math.max(1, Math.round(Math.sqrt(total / 2)) || 1);
  const cols = Math.min(256, side);
  const rows = Math.min(256, side);
  const cellW = (maxX - minX) / cols || 1;
  const cellH = (maxY - minY) / rows || 1;
  const cells: number[][] = Array.from({ length: cols * rows }, () => []);
  for (let si = 0; si < series.length; si++) {
    const s = series[si];
    const n = Math.min(s.x.length, s.y.length);
    for (let i = 0; i < n; i++) {
      const cxi = Math.min(cols - 1, Math.max(0, Math.floor((s.x[i] - minX) / cellW)));
      const cyi = Math.min(rows - 1, Math.max(0, Math.floor((s.y[i] - minY) / cellH)));
      cells[cyi * cols + cxi].push(si, i);
    }
  }
  return { minX, minY, cellW, cellH, cols, rows, cells };
}

// --- shared canvas figure drawing -------------------------------------------

export interface DrawFigureOpts {
  series: Series[];
  xr: [number, number];
  yr: [number, number];
  rect: PlotRectPx; // plot area, in the ctx's logical (pre-scaled) px
  fontPx: number;
  labelFontPx?: number; // gene-label font (defaults to fontPx)
  axisWidthPx: number;
  markerRadiusPx?: number; // uniform override; else per-series radius
  xTitle: string;
  yTitle: string;
  shapes: LineShapeLite[];
  labels: LabelPoint[];
  manualOffsets?: Record<string, ManualOffset>;
  placements?: LabelPlacement[]; // precomputed label boxes (else computed here)
  skipDots?: boolean; // skip the scatter (caller blits a cached dots layer)
  viewport?: { left: number; top: number; right: number; bottom: number }; // cull dots to this rect (px)
  xDtick?: number;
  yDtick?: number;
  selectedGene?: string | null; // gene whose label/anchor to highlight
}

const TICK_LEN = 5;
const AXIS_COLOR = "rgb(51,51,51)";
const LABEL_COLOR = "#111";

/**
 * Draw the whole figure (cutoff lines, dots, axis frame + ticks + labels +
 * titles, repel gene labels) into `ctx`. All sizes are in the ctx's current
 * logical units; the caller pre-scales the context (devicePixelRatio for the
 * screen, dpi/96 for PNG export).
 */
export function drawFigure(ctx: CanvasRenderingContext2D, o: DrawFigureOpts): LabelPlacement[] {
  const { rect, fontPx } = o;
  const left = rect.left;
  const top = rect.top;
  const right = rect.left + rect.width;
  const bottom = rect.top + rect.height;
  const dx = o.xr[1] - o.xr[0] || 1;
  const dy = o.yr[1] - o.yr[0] || 1;
  const mapX = (x: number) => left + ((x - o.xr[0]) / dx) * rect.width;
  const mapY = (y: number) => top + (1 - (y - o.yr[0]) / dy) * rect.height;

  // Cutoff / reference lines, clipped to the plot rectangle.
  for (const sh of o.shapes) {
    if (sh.type && sh.type !== "line") continue;
    const X = (v: number) => (sh.xref === "paper" ? left + v * rect.width : mapX(v));
    const Y = (v: number) => (sh.yref === "paper" ? top + (1 - v) * rect.height : mapY(v));
    let x1: number, y1: number, x2: number, y2: number;
    if (sh.x0 === sh.x1) {
      x1 = x2 = X(sh.x0 ?? 0);
      if (x1 < left - 0.5 || x1 > right + 0.5) continue;
      y1 = top; y2 = bottom;
    } else if (sh.y0 === sh.y1) {
      y1 = y2 = Y(sh.y0 ?? 0);
      if (y1 < top - 0.5 || y1 > bottom + 0.5) continue;
      x1 = left; x2 = right;
    } else {
      x1 = X(sh.x0 ?? 0); y1 = Y(sh.y0 ?? 0); x2 = X(sh.x1 ?? 0); y2 = Y(sh.y1 ?? 0);
    }
    ctx.save();
    ctx.strokeStyle = sh.line?.color ?? "rgba(127,127,127,0.4)";
    ctx.lineWidth = sh.line?.width ?? 1;
    ctx.setLineDash(sh.line?.dash === "dot" ? [ctx.lineWidth, ctx.lineWidth * 2] : []);
    ctx.beginPath();
    ctx.moveTo(x1, y1);
    ctx.lineTo(x2, y2);
    ctx.stroke();
    ctx.restore();
  }

  // Dots, clipped to the plot rectangle. (May be skipped when the caller blits
  // a cached dots layer for performance.)
  if (!o.skipDots) {
    ctx.save();
    ctx.beginPath();
    ctx.rect(left, top, rect.width, rect.height);
    ctx.clip();
    // Cull to the viewport if given (a magnified preview only needs the visible
    // dots), else to the plot rectangle.
    const vp = o.viewport ?? { left, top, right, bottom };
    for (const svy of o.series) {
      ctx.fillStyle = svy.color;
      const r = o.markerRadiusPx ?? svy.radius;
      const n = Math.min(svy.x.length, svy.y.length);
      for (let i = 0; i < n; i++) {
        const px = mapX(svy.x[i]);
        const py = mapY(svy.y[i]);
        if (px < vp.left - r || px > vp.right + r || py < vp.top - r || py > vp.bottom + r) continue;
        ctx.beginPath();
        ctx.arc(px, py, r, 0, 2 * Math.PI);
        ctx.fill();
      }
    }
    ctx.restore();
  }

  // Axis frame + tick marks.
  const xt = axisTicks(o.xr[0], o.xr[1], o.xDtick);
  const yt = axisTicks(o.yr[0], o.yr[1], o.yDtick);
  ctx.save();
  ctx.strokeStyle = AXIS_COLOR;
  ctx.lineWidth = o.axisWidthPx;
  ctx.lineCap = "square";
  ctx.beginPath();
  ctx.moveTo(left, top); ctx.lineTo(left, bottom); // y axis
  ctx.lineTo(right, bottom); // x axis
  for (const t of xt.ticks) {
    const px = mapX(t);
    ctx.moveTo(px, bottom); ctx.lineTo(px, bottom + TICK_LEN);
  }
  for (const t of yt.ticks) {
    const py = mapY(t);
    ctx.moveTo(left, py); ctx.lineTo(left - TICK_LEN, py);
  }
  ctx.stroke();
  ctx.restore();

  // Tick labels + axis titles.
  ctx.save();
  ctx.fillStyle = LABEL_COLOR;
  ctx.font = `${fontPx}px ${PLOT_FONT_FAMILY}`;
  const xDec = tickDecimals(xt.step);
  const yDec = tickDecimals(yt.step);
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (const t of xt.ticks) {
    ctx.fillText(fmtTick(t, xDec), mapX(t), bottom + TICK_LEN + 2);
  }
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (const t of yt.ticks) {
    ctx.fillText(fmtTick(t, yDec), left - TICK_LEN - 3, mapY(t));
  }
  if (o.xTitle) {
    ctx.textAlign = "center";
    ctx.textBaseline = "alphabetic";
    ctx.fillText(o.xTitle, left + rect.width / 2, bottom + TICK_LEN + fontPx * 2.3);
  }
  if (o.yTitle) {
    ctx.save();
    ctx.translate(fontPx * 0.95, top + rect.height / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(o.yTitle, 0, 0);
    ctx.restore();
  }
  ctx.restore();

  // Repel-placed gene labels (leader line + white box + text).
  const labelFontPx = o.labelFontPx ?? fontPx;
  const placements = o.placements ?? layoutLabels(o.labels, rect, o.xr, o.yr, labelFontPx, o.manualOffsets);
  if (placements.length) {
    ctx.save();
    ctx.font = `${labelFontPx}px ${PLOT_FONT_FAMILY}`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    const leaderW = Math.max(0.4, o.axisWidthPx * 0.8);
    for (const p of placements) {
      const selected = !!o.selectedGene && p.gene === o.selectedGene;
      const lp = leaderPoint(p.anchor.x, p.anchor.y, p.center.x, p.center.y, p.w, p.h);
      if (selected) {
        // Highlight box behind the selected label + a ring on its dot, so the
        // user sees what Delete will remove.
        const padX = 3;
        const padY = 2;
        ctx.fillStyle = "rgba(0,114,178,0.15)";
        ctx.strokeStyle = "#0072B2";
        ctx.lineWidth = Math.max(0.6, o.axisWidthPx);
        ctx.setLineDash([]);
        ctx.beginPath();
        ctx.rect(p.center.x - p.w / 2 - padX, p.center.y - p.h / 2 - padY, p.w + 2 * padX, p.h + 2 * padY);
        ctx.fill();
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(p.anchor.x, p.anchor.y, Math.max(2.5, labelFontPx * 0.5), 0, 2 * Math.PI);
        ctx.stroke();
      }
      ctx.strokeStyle = "rgba(80,80,80,0.7)";
      ctx.lineWidth = leaderW;
      ctx.setLineDash([]);
      ctx.beginPath();
      ctx.moveTo(p.anchor.x, p.anchor.y);
      ctx.lineTo(lp.x, lp.y);
      ctx.stroke();
      ctx.fillStyle = LABEL_COLOR;
      ctx.fillText(p.label ?? p.gene, p.center.x, p.center.y);
    }
    ctx.restore();
  }
  return placements;
}
