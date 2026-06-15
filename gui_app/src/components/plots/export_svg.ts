// Self-contained publication SVG export (no Plotly). We draw the chrome
// ourselves — axis frame, 1-2-5 "nice" ticks + labels, axis titles, reference
// (cutoff) lines and the repel-placed gene labels — as plain SVG with pt-sized
// fonts (px = pt*96/72 so PowerPoint keeps the size after Ungroup), and embed
// the scatter as a high-DPI PNG rendered on an offscreen canvas. This gives
// full control over geometry (perfect tick/axis/data alignment), keeps the SVG
// flat/ungroup-friendly, and makes the preview identical to the saved file.

import {
  layoutLabels,
  PLOT_FONT_FAMILY,
  type ExportSize,
  type LabelPoint,
} from "./types";
import { axisTicks, drawFigure, fmtTick, leaderPoint, tickDecimals, type LineShapeLite, type Series } from "./plot_core";

const escapeXml = (s: string) =>
  s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");

const MM_PER_IN = 25.4;
// SVG user units = CSS px at 96 dpi. We size the viewBox in these units and set
// font-size in px (= pt * 96/72). PowerPoint reads font-size as px at 96 dpi,
// so both "grouped" and "Ungroup" yield the intended pt.
const CSS_DPI = 96;
const mmToPx = (mm: number) => (mm / MM_PER_IN) * CSS_DPI;
const ptToPx = (pt: number) => (pt * CSS_DPI) / 72;

// The export range comes from the explicit spec (the settings panel's X/Y
// range) or, if unset, the full data extent. We deliberately IGNORE any range
// on the live layout — that follows the user's interactive zoom.
export function resolveRanges(
  specX: [number, number] | null | undefined,
  specY: [number, number] | null | undefined,
  data: { x?: number[]; y?: number[] }[],
): { xr: [number, number]; yr: [number, number] } {
  let xmin = Infinity, xmax = -Infinity, ymin = Infinity, ymax = -Infinity;
  for (const t of data) {
    for (const v of t.x ?? []) {
      if (v < xmin) xmin = v;
      if (v > xmax) xmax = v;
    }
    for (const v of t.y ?? []) {
      if (v < ymin) ymin = v;
      if (v > ymax) ymax = v;
    }
  }
  const padX = (xmax - xmin) * 0.05 || 1;
  const padY = (ymax - ymin) * 0.05 || 1;
  const yBottom = ymin >= 0 ? 0 : ymin - padY;
  const xr = specX ?? [xmin - padX, xmax + padX];
  const yr = specY ?? [yBottom, ymax + padY];
  return { xr, yr };
}

// --- offscreen scatter raster ------------------------------------------------

function renderDotsPng(
  series: Series[],
  xr: [number, number],
  yr: [number, number],
  plotW: number,
  plotH: number,
  scale: number,
  markerDiaPx: number,
): string {
  const cw = Math.max(1, Math.round(plotW * scale));
  const ch = Math.max(1, Math.round(plotH * scale));
  const canvas = document.createElement("canvas");
  canvas.width = cw;
  canvas.height = ch;
  const ctx = canvas.getContext("2d");
  if (!ctx) return "";
  const r = Math.max(0.4, (markerDiaPx / 2) * scale);
  const dx = xr[1] - xr[0] || 1;
  const dy = yr[1] - yr[0] || 1;
  for (const sv of series) {
    ctx.fillStyle = sv.color;
    const n = Math.min(sv.x.length, sv.y.length);
    for (let i = 0; i < n; i++) {
      const px = ((sv.x[i] - xr[0]) / dx) * cw;
      const py = (1 - (sv.y[i] - yr[0]) / dy) * ch;
      ctx.beginPath();
      ctx.arc(px, py, r, 0, 2 * Math.PI);
      ctx.fill();
    }
  }
  return canvas.toDataURL("image/png");
}

// --- shared geometry ---------------------------------------------------------

export function exportGeometry(size: ExportSize) {
  const dpi = size.dpi ?? 300;
  const fontPx = ptToPx(size.fontPt ?? 12);
  const labelFontPx = ptToPx(size.labelFontPt ?? 5);
  const axisWidthPx = ptToPx(size.axisWidthPt ?? 0.75);
  // Integer plot rectangle so the dots raster and the chrome align exactly.
  const plotW = Math.max(1, Math.round(mmToPx(size.widthMm ?? 90)));
  const plotH = Math.max(1, Math.round(mmToPx(size.heightMm ?? 70)));
  const m = {
    l: Math.round(fontPx * 5),
    r: Math.round(fontPx * 2),
    t: Math.round(fontPx * 2),
    b: Math.round(fontPx * 4),
  };
  const vbW = plotW + m.l + m.r;
  const vbH = plotH + m.t + m.b;
  const markerDiaPx = size.pointRadiusMm
    ? Math.max(0.5, (2 * size.pointRadiusMm) / MM_PER_IN * CSS_DPI)
    : 4;
  return {
    dpi,
    fontPx,
    labelFontPx,
    axisWidthPx,
    plotW,
    plotH,
    m,
    vbW,
    vbH,
    totalMmW: (vbW / CSS_DPI) * MM_PER_IN,
    totalMmH: (vbH / CSS_DPI) * MM_PER_IN,
    dotsScale: dpi / CSS_DPI,
    markerDiaPx,
  };
}

function readTitle(layout: Record<string, unknown>, axis: "xaxis" | "yaxis"): string {
  const ax = layout[axis] as { title?: { text?: string } } | undefined;
  return ax?.title?.text ?? "";
}

function canvasToPng(canvas: HTMLCanvasElement): Promise<Uint8Array> {
  return new Promise((resolve, reject) => {
    canvas.toBlob((blob) => {
      if (!blob) {
        reject(new Error("canvas toBlob failed"));
        return;
      }
      blob.arrayBuffer().then((buf) => resolve(new Uint8Array(buf)), reject);
    }, "image/png");
  });
}

/** Full-figure PNG raster (no Plotly), drawn with the SAME geometry as the SVG. */
export async function renderFigurePng(
  baseLayout: Record<string, unknown>,
  series: Series[],
  size: ExportSize,
  labels: LabelPoint[] = [],
): Promise<Uint8Array> {
  const g = exportGeometry(size);
  const { xr, yr } = resolveRanges(size.xRange, size.yRange, series);
  const canvas = document.createElement("canvas");
  canvas.width = Math.max(1, Math.round(g.vbW * g.dotsScale));
  canvas.height = Math.max(1, Math.round(g.vbH * g.dotsScale));
  const ctx = canvas.getContext("2d");
  if (!ctx) throw new Error("no 2d context");
  ctx.scale(g.dotsScale, g.dotsScale);
  drawFigure(ctx, {
    series,
    xr,
    yr,
    rect: { left: g.m.l, top: g.m.t, width: g.plotW, height: g.plotH },
    fontPx: g.fontPx,
    labelFontPx: g.labelFontPx,
    axisWidthPx: g.axisWidthPx,
    markerRadiusPx: g.markerDiaPx / 2,
    xTitle: readTitle(baseLayout, "xaxis"),
    yTitle: readTitle(baseLayout, "yaxis"),
    shapes: (baseLayout.shapes as LineShapeLite[] | undefined) ?? [],
    labels,
    manualOffsets: size.manualOffsets,
    xDtick: size.xDtick,
    yDtick: size.yDtick,
  });
  return canvasToPng(canvas);
}

// --- main SVG ----------------------------------------------------------------

export async function renderHybridSvg(
  baseLayout: Record<string, unknown>,
  series: Series[],
  size: ExportSize,
  labels: LabelPoint[] = [],
): Promise<Uint8Array> {
  const g = exportGeometry(size);
  const { fontPx, labelFontPx, axisWidthPx, plotW, plotH, m, vbW, vbH, totalMmW, totalMmH, dotsScale, markerDiaPx } = g;

  const { xr, yr } = resolveRanges(size.xRange, size.yRange, series);

  const left = m.l;
  const right = m.l + plotW;
  const top = m.t;
  const bottom = m.t + plotH;
  const dx = xr[1] - xr[0] || 1;
  const dy = yr[1] - yr[0] || 1;
  const mapX = (x: number) => left + ((x - xr[0]) / dx) * plotW;
  const mapY = (y: number) => top + (1 - (y - yr[0]) / dy) * plotH;

  const dotsUrl = renderDotsPng(series, xr, yr, plotW, plotH, dotsScale, markerDiaPx);

  const xt = axisTicks(xr[0], xr[1], size.xDtick);
  const yt = axisTicks(yr[0], yr[1], size.yDtick);
  const xDec = tickDecimals(xt.step);
  const yDec = tickDecimals(yt.step);

  const tickLen = 5;
  const frameStroke = `fill:none;stroke:rgb(51,51,51);stroke-width:${axisWidthPx}px;stroke-linecap:square`;
  const tickFont = `font-family:${PLOT_FONT_FAMILY};font-size:${fontPx}px;fill:#111`;

  const xmlns = 'xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"';
  const dataAttrs =
    `data-plot-left="${left}" data-plot-top="${top}" data-plot-w="${plotW}" data-plot-h="${plotH}" ` +
    `data-xr0="${xr[0]}" data-xr1="${xr[1]}" data-yr0="${yr[0]}" data-yr1="${yr[1]}"`;
  let s = `<svg ${xmlns} ${dataAttrs} viewBox="0 0 ${vbW} ${vbH}" width="${totalMmW}mm" height="${totalMmH}mm">`;

  // Data raster, clipped to the plot rectangle.
  s += `<image x="${left}" y="${top}" width="${plotW}" height="${plotH}" preserveAspectRatio="none" xlink:href="${dotsUrl}"/>`;

  // Reference (cutoff) lines.
  const shapes = (baseLayout.shapes as LineShapeLite[] | undefined) ?? [];
  if (shapes.length) {
    s += '<g class="pub-shapes">';
    for (const sh of shapes) {
      if (sh.type && sh.type !== "line") continue;
      const col = sh.line?.color ?? "rgba(127,127,127,0.4)";
      const w = sh.line?.width ?? 1;
      const dash = sh.line?.dash === "dot" ? `;stroke-dasharray:${w},${w * 2}` : "";
      const X = (v: number) => (sh.xref === "paper" ? left + v * plotW : mapX(v));
      const Y = (v: number) => (sh.yref === "paper" ? top + (1 - v) * plotH : mapY(v));
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
      s += `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" style="stroke:${col};stroke-width:${w}px${dash}"/>`;
    }
    s += "</g>";
  }

  // L-frame + tick marks.
  s += '<g class="pub-axes">';
  s += `<path d="M${left},${top}V${bottom}" style="${frameStroke}"/>`;
  s += `<path d="M${left},${bottom}H${right}" style="${frameStroke}"/>`;
  for (const t of xt.ticks) {
    const px = mapX(t);
    s += `<path d="M${px},${bottom}V${bottom + tickLen}" style="${frameStroke}"/>`;
  }
  for (const t of yt.ticks) {
    const py = mapY(t);
    s += `<path d="M${left},${py}H${left - tickLen}" style="${frameStroke}"/>`;
  }
  s += "</g>";

  // Tick labels.
  s += '<g class="pub-ticklabels">';
  for (const t of xt.ticks) {
    s += `<text x="${mapX(t)}" y="${bottom + tickLen + fontPx}" text-anchor="middle" style="${tickFont}">${fmtTick(t, xDec)}</text>`;
  }
  for (const t of yt.ticks) {
    s += `<text x="${left - tickLen - 3}" y="${mapY(t) + fontPx * 0.32}" text-anchor="end" style="${tickFont}">${fmtTick(t, yDec)}</text>`;
  }
  s += "</g>";

  // Axis titles.
  const xTitle = readTitle(baseLayout, "xaxis");
  const yTitle = readTitle(baseLayout, "yaxis");
  if (xTitle) {
    const ty = bottom + tickLen + fontPx + fontPx * 1.3;
    s += `<text x="${left + plotW / 2}" y="${ty}" text-anchor="middle" style="${tickFont}">${escapeXml(xTitle)}</text>`;
  }
  if (yTitle) {
    const tx = fontPx * 0.95;
    const tyc = top + plotH / 2;
    s += `<text x="${tx}" y="${tyc}" text-anchor="middle" transform="rotate(-90 ${tx} ${tyc})" style="${tickFont}">${escapeXml(yTitle)}</text>`;
  }

  // Gene labels: one shallow <g class="pub-label"> per gene (leader + bg + text),
  // tagged with the gene + its data coords so the inline-SVG editor can drag
  // them and convert the drop back to a data-space offset.
  const placements = layoutLabels(
    labels,
    { left, top, width: plotW, height: plotH },
    xr,
    yr,
    labelFontPx,
    size.manualOffsets,
  );
  if (placements.length > 0) {
    const leaderW = Math.max(0.4, axisWidthPx * 0.8);
    const leaderStyle = `fill:none;stroke:rgba(80,80,80,0.7);stroke-width:${leaderW}px`;
    const textStyle = `font-family:${PLOT_FONT_FAMILY};font-size:${labelFontPx}px;fill:#111`;
    s += '<g class="pub-labels">';
    for (const p of placements) {
      const cx = p.center.x;
      const cy = p.center.y;
      const lp = leaderPoint(p.anchor.x, p.anchor.y, cx, cy, p.w, p.h);
      const g = escapeXml(p.gene); // identity (gene id)
      const txt = escapeXml(p.label ?? p.gene); // rendered text (symbol when set)
      s +=
        `<g class="pub-label" data-gene="${g}" data-datax="${p.x}" data-datay="${p.y}">` +
        `<line class="pub-leader" x1="${p.anchor.x}" y1="${p.anchor.y}" x2="${lp.x}" y2="${lp.y}" style="${leaderStyle}"/>` +
        `<text class="pub-text" x="${cx}" y="${cy + labelFontPx * 0.35}" text-anchor="middle" style="${textStyle}">${txt}</text>` +
        `</g>`;
    }
    s += "</g>";
  }

  s += "</svg>";
  return new TextEncoder().encode(s);
}
