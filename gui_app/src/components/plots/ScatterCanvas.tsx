// Interactive scatter, drawn on a 2D canvas (no Plotly). Pan (drag), zoom
// (wheel toward cursor), reset (double-click), hover tooltip and click-to-label.
// Shares the drawing core (drawFigure) and tick math with the export, so the
// on-screen view, the SVG export and the PNG export all agree.

import { useCallback, useEffect, useMemo, useRef } from "react";
import { buildHitIndex, drawFigure, type HitIndex, type LineShapeLite, type Series } from "./plot_core";
import { renderFigurePng, renderHybridSvg, resolveRanges } from "./export_svg";
import {
  PLOT_FONT_FAMILY,
  SCREEN_FONT,
  type ExportFn,
  type ExportFormat,
  type ExportSize,
  type LabelPlacement,
  type LabelPoint,
} from "./types";

const MARGIN = { l: 52, r: 16, t: 14, b: 44 }; // CSS px, room for ticks + titles
const HIT_PX = 8; // hover/click pick radius

export interface ScatterCanvasProps {
  series: Series[];
  xTitle: string;
  yTitle: string;
  shapes: LineShapeLite[];
  labelPoints: LabelPoint[];
  specXRange: [number, number] | null;
  specYRange: [number, number] | null;
  revision: number;
  hoverFormat: (gene: string, x: number, y: number) => string;
  onHover?: (gene: string | null) => void;
  onToggleLabel?: (gene: string) => void;
  selectedGene?: string | null;
  onSelectGene?: (gene: string | null) => void;
  onRendered?: (ms: number) => void;
  registerExport?: (fn: ExportFn | null) => void;
}

export function ScatterCanvas(props: ScatterCanvasProps) {
  const {
    series, xTitle, yTitle, shapes, labelPoints,
    specXRange, specYRange, revision, hoverFormat,
    onHover, onToggleLabel, selectedGene, onSelectGene, onRendered, registerExport,
  } = props;

  const canvasRef = useRef<HTMLCanvasElement>(null);
  const boxRef = useRef<HTMLDivElement>(null);
  const tipRef = useRef<HTMLDivElement>(null);
  const viewRef = useRef<{ xr: [number, number]; yr: [number, number] }>({ xr: [0, 1], yr: [0, 1] });
  const rectRef = useRef({ left: MARGIN.l, top: MARGIN.t, width: 1, height: 1 });
  const rafRef = useRef<number | undefined>(undefined);

  // Latest inputs via refs so the stable export/draw closures read current data.
  const seriesRef = useRef(series); seriesRef.current = series;
  const labelsRef = useRef(labelPoints); labelsRef.current = labelPoints;
  const shapesRef = useRef(shapes); shapesRef.current = shapes;
  const cbRef = useRef({ onHover, onToggleLabel, onSelectGene, hoverFormat });
  cbRef.current = { onHover, onToggleLabel, onSelectGene, hoverFormat };
  const selRef = useRef(selectedGene);
  selRef.current = selectedGene;
  const placementsRef = useRef<LabelPlacement[]>([]);

  const index = useMemo<HitIndex>(() => buildHitIndex(series), [series]);
  const indexRef = useRef(index); indexRef.current = index;

  const draw = useCallback(() => {
    const canvas = canvasRef.current;
    const box = boxRef.current;
    if (!canvas || !box) return;
    const dpr = window.devicePixelRatio || 1;
    const cw = box.clientWidth;
    const ch = box.clientHeight;
    if (canvas.width !== Math.round(cw * dpr) || canvas.height !== Math.round(ch * dpr)) {
      canvas.width = Math.round(cw * dpr);
      canvas.height = Math.round(ch * dpr);
      canvas.style.width = `${cw}px`;
      canvas.style.height = `${ch}px`;
    }
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cw, ch);
    const rect = {
      left: MARGIN.l,
      top: MARGIN.t,
      width: Math.max(1, cw - MARGIN.l - MARGIN.r),
      height: Math.max(1, ch - MARGIN.t - MARGIN.b),
    };
    rectRef.current = rect;
    const v = viewRef.current;
    placementsRef.current = drawFigure(ctx, {
      series: seriesRef.current,
      xr: v.xr,
      yr: v.yr,
      rect,
      fontPx: SCREEN_FONT,
      axisWidthPx: 1,
      xTitle,
      yTitle,
      shapes: shapesRef.current,
      labels: labelsRef.current,
      selectedGene: selRef.current,
    });
  }, [xTitle, yTitle]);

  const scheduleDraw = useCallback(() => {
    if (rafRef.current !== undefined) return;
    rafRef.current = requestAnimationFrame(() => {
      rafRef.current = undefined;
      draw();
    });
  }, [draw]);

  const resetView = useCallback(() => {
    // Interactive Fit is ALWAYS auto-computed from the data extent. It does not
    // follow the publication (spec) x/y range — that only governs the export
    // and the Publication-mode preview.
    viewRef.current = resolveRanges(null, null, seriesRef.current);
  }, []);

  // Fit the view on first mount / revision change.
  useEffect(() => {
    resetView();
    scheduleDraw();
  }, [resetView, revision, scheduleDraw]);

  // Redraw when the figure content changes. Re-fit the view only when the data
  // EXTENT changes (a new dataset) — not on cutoff tweaks or pan/zoom — so the
  // first load fits and the zoom survives content changes.
  const lastExtentRef = useRef("");
  useEffect(() => {
    const key = JSON.stringify(resolveRanges(null, null, series));
    if (key !== lastExtentRef.current) {
      lastExtentRef.current = key;
      resetView();
    }
    scheduleDraw();
    const t0 = performance.now();
    const id = requestAnimationFrame(() => onRendered?.(performance.now() - t0));
    return () => cancelAnimationFrame(id);
  }, [series, labelPoints, shapes, resetView, scheduleDraw, onRendered]);

  // Resize handling.
  useEffect(() => {
    const box = boxRef.current;
    if (!box) return;
    const ro = new ResizeObserver(() => scheduleDraw());
    ro.observe(box);
    return () => ro.disconnect();
  }, [scheduleDraw]);

  // Screen px → data coords.
  const toData = useCallback((clientX: number, clientY: number) => {
    const box = boxRef.current!;
    const r = box.getBoundingClientRect();
    const px = clientX - r.left;
    const py = clientY - r.top;
    const rect = rectRef.current;
    const v = viewRef.current;
    const x = v.xr[0] + ((px - rect.left) / rect.width) * (v.xr[1] - v.xr[0]);
    const y = v.yr[0] + (1 - (py - rect.top) / rect.height) * (v.yr[1] - v.yr[0]);
    return { px, py, x, y };
  }, []);

  // Nearest point within HIT_PX (screen). Returns the series/point or null.
  const pick = useCallback((clientX: number, clientY: number) => {
    const { px, py } = toData(clientX, clientY);
    const rect = rectRef.current;
    const v = viewRef.current;
    const idx = indexRef.current;
    const scaleX = rect.width / (v.xr[1] - v.xr[0] || 1);
    const scaleY = rect.height / (v.yr[1] - v.yr[0] || 1);
    const dataX = v.xr[0] + ((px - rect.left) / rect.width) * (v.xr[1] - v.xr[0]);
    const dataY = v.yr[0] + (1 - (py - rect.top) / rect.height) * (v.yr[1] - v.yr[0]);
    const rx = HIT_PX / scaleX;
    const ry = HIT_PX / scaleY;
    const c0 = Math.max(0, Math.floor((dataX - rx - idx.minX) / idx.cellW));
    const c1 = Math.min(idx.cols - 1, Math.floor((dataX + rx - idx.minX) / idx.cellW));
    const r0 = Math.max(0, Math.floor((dataY - ry - idx.minY) / idx.cellH));
    const r1 = Math.min(idx.rows - 1, Math.floor((dataY + ry - idx.minY) / idx.cellH));
    let best: { si: number; pi: number } | null = null;
    let bestD = HIT_PX * HIT_PX;
    const ss = seriesRef.current;
    for (let cy = r0; cy <= r1; cy++) {
      for (let cx = c0; cx <= c1; cx++) {
        const cell = idx.cells[cy * idx.cols + cx];
        for (let k = 0; k < cell.length; k += 2) {
          const si = cell[k];
          const pi = cell[k + 1];
          const s = ss[si];
          const sx = rect.left + ((s.x[pi] - v.xr[0]) / (v.xr[1] - v.xr[0] || 1)) * rect.width;
          const sy = rect.top + (1 - (s.y[pi] - v.yr[0]) / (v.yr[1] - v.yr[0] || 1)) * rect.height;
          const d = (sx - px) * (sx - px) + (sy - py) * (sy - py);
          if (d <= bestD) { bestD = d; best = { si, pi }; }
        }
      }
    }
    return best ? { gene: ss[best.si].ids[best.pi], x: ss[best.si].x[best.pi], y: ss[best.si].y[best.pi], px, py } : null;
  }, [toData]);

  // Hit-test the on-plot gene LABELS (boxes from the last draw). Returns the
  // gene whose label box contains the click, else null.
  const pickLabel = useCallback((clientX: number, clientY: number): string | null => {
    const box = boxRef.current;
    if (!box) return null;
    const r = box.getBoundingClientRect();
    const px = clientX - r.left;
    const py = clientY - r.top;
    for (const p of placementsRef.current) {
      if (Math.abs(px - p.center.x) <= p.w / 2 + 3 && Math.abs(py - p.center.y) <= p.h / 2 + 2) return p.gene;
    }
    return null;
  }, []);

  // Redraw when the selection changes (updates the highlight).
  useEffect(() => { scheduleDraw(); }, [selectedGene, scheduleDraw]);

  // --- pointer interaction ---
  const dragRef = useRef<{ x: number; y: number; xr: [number, number]; yr: [number, number]; moved: boolean } | null>(null);

  const onPointerDown = (e: React.PointerEvent) => {
    const v = viewRef.current;
    dragRef.current = { x: e.clientX, y: e.clientY, xr: [...v.xr], yr: [...v.yr], moved: false };
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
  };
  const onPointerMove = (e: React.PointerEvent) => {
    const d = dragRef.current;
    if (d) {
      const dxp = e.clientX - d.x;
      const dyp = e.clientY - d.y;
      if (Math.abs(dxp) + Math.abs(dyp) > 3) d.moved = true;
      const rect = rectRef.current;
      const xspan = d.xr[1] - d.xr[0];
      const yspan = d.yr[1] - d.yr[0];
      const ddx = (dxp / rect.width) * xspan;
      const ddy = (dyp / rect.height) * yspan;
      viewRef.current = {
        xr: [d.xr[0] - ddx, d.xr[1] - ddx],
        yr: [d.yr[0] + ddy, d.yr[1] + ddy], // screen y is inverted
      };
      scheduleDraw();
      return;
    }
    // Hover.
    const hit = pick(e.clientX, e.clientY);
    const tip = tipRef.current;
    cbRef.current.onHover?.(hit ? hit.gene : null);
    if (hit && tip) {
      tip.style.display = "block";
      tip.style.left = `${hit.px + 12}px`;
      tip.style.top = `${hit.py + 12}px`;
      tip.textContent = cbRef.current.hoverFormat(hit.gene, hit.x, hit.y);
    } else if (tip) {
      tip.style.display = "none";
    }
  };
  const endDrag = (e: React.PointerEvent) => {
    const d = dragRef.current;
    dragRef.current = null;
    if (d && !d.moved) {
      // Click a gene LABEL → select it. Else click a DOT → toggle its label.
      // Else (empty space) → clear the selection.
      const lab = pickLabel(e.clientX, e.clientY);
      if (lab) { cbRef.current.onSelectGene?.(lab); return; }
      const hit = pick(e.clientX, e.clientY);
      if (hit) { cbRef.current.onToggleLabel?.(hit.gene); return; }
      cbRef.current.onSelectGene?.(null);
    }
  };
  const onPointerLeave = () => {
    if (tipRef.current) tipRef.current.style.display = "none";
    cbRef.current.onHover?.(null);
  };

  // Wheel zoom toward the cursor (native listener for preventDefault).
  useEffect(() => {
    const box = boxRef.current;
    if (!box) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const { x, y } = toData(e.clientX, e.clientY);
      const factor = e.deltaY < 0 ? 1 / 1.15 : 1.15; // up = zoom in (shrink range)
      const v = viewRef.current;
      viewRef.current = {
        xr: [x - (x - v.xr[0]) * factor, x + (v.xr[1] - x) * factor],
        yr: [y - (y - v.yr[0]) * factor, y + (v.yr[1] - y) * factor],
      };
      scheduleDraw();
    };
    box.addEventListener("wheel", onWheel, { passive: false });
    return () => box.removeEventListener("wheel", onWheel);
  }, [toData, scheduleDraw]);

  // --- export (SVG + PNG, no Plotly) ---
  const exportFn = useCallback<ExportFn>(async (format: ExportFormat, size?: ExportSize) => {
    const baseLayout = {
      xaxis: { title: { text: xTitle } },
      yaxis: { title: { text: yTitle } },
      shapes: shapesRef.current,
    } as Record<string, unknown>;
    const sz: ExportSize = size ?? {
      width: 1200, height: 800, widthMm: 120, heightMm: 80, dpi: 150,
      fontPt: 12, pointRadiusMm: 0, axisWidthPt: 0.75,
      xRange: specXRange, yRange: specYRange,
    };
    if (format === "svg") {
      return renderHybridSvg(baseLayout, seriesRef.current, sz, labelsRef.current);
    }
    return renderFigurePng(baseLayout, seriesRef.current, sz, labelsRef.current);
  }, [xTitle, yTitle, specXRange, specYRange]);

  useEffect(() => {
    registerExport?.(exportFn);
    return () => registerExport?.(null);
  }, [registerExport, exportFn]);

  return (
    <div
      ref={boxRef}
      style={{ position: "relative", width: "100%", height: "100%", overflow: "hidden", cursor: "crosshair", touchAction: "none" }}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={endDrag}
      onPointerLeave={onPointerLeave}
      onDoubleClick={() => { resetView(); scheduleDraw(); }}
    >
      <canvas ref={canvasRef} style={{ display: "block" }} />
      <button
        onPointerDown={(e) => e.stopPropagation()} // don't let the box capture the pointer
        onClick={() => { resetView(); scheduleDraw(); }}
        title="Fit to data"
        style={{ position: "absolute", top: 6, right: 6, fontSize: 11, padding: "1px 8px", opacity: 0.85 }}
      >
        Fit
      </button>
      <div
        ref={tipRef}
        style={{
          position: "absolute", display: "none", pointerEvents: "none",
          background: "rgba(255,255,255,0.95)", border: "1px solid rgba(0,0,0,0.2)",
          borderRadius: 3, padding: "2px 6px", fontSize: 11, fontFamily: PLOT_FONT_FAMILY,
          whiteSpace: "pre", color: "#111", zIndex: 5,
        }}
      />
    </div>
  );
}
