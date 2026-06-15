// Canvas-based export preview. Draws the figure with the SAME engine/geometry
// as the export (drawFigure at the mm/pt/dpi spec), so it matches Save exactly,
// but live: hover tooltip, click a dot to toggle its label, drag a gene label
// to place it, and a magnifier (wheel zoom / drag pan) for inspection — the
// magnifier scales the whole figure (dots included) and does NOT change the
// export. Dots are rasterized only at Save time (renderHybridSvg/renderFigurePng).

import { useCallback, useEffect, useMemo, useRef } from "react";
import { buildHitIndex, drawFigure, type HitIndex, type LineShapeLite, type Series } from "./plot_core";
import { exportGeometry, resolveRanges } from "./export_svg";
import {
  layoutLabels,
  PLOT_FONT_FAMILY,
  type ExportSize,
  type LabelPlacement,
  type LabelPoint,
  type ManualOffset,
} from "./types";

const HIT_PX = 8;

export interface ExportPreviewProps {
  series: Series[];
  xTitle: string;
  yTitle: string;
  shapes: LineShapeLite[];
  labelPoints: LabelPoint[];
  hoverFormat: (gene: string, x: number, y: number) => string;
  size: ExportSize;
  manualOffsets: Record<string, ManualOffset>;
  onManualMove: (gene: string, off: ManualOffset) => void;
  onToggleLabel: (gene: string) => void;
  onHover?: (gene: string | null) => void;
  selectedGene?: string | null;
  onSelectGene?: (gene: string | null) => void;
  fitTick?: number; // bump to fit-to-window (Fit button lives in the section header)
}

export function ExportPreview(props: ExportPreviewProps) {
  const { series, xTitle, yTitle, shapes, labelPoints, hoverFormat, size, manualOffsets, onManualMove, onToggleLabel, onHover, selectedGene, onSelectGene, fitTick } = props;

  const boxRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const tipRef = useRef<HTMLDivElement>(null);
  const rafRef = useRef<number | undefined>(undefined);

  // Depend only on the fields exportGeometry actually reads. Keying on the
  // whole `size` object would recompute geo (new identity) when unrelated
  // fields change — e.g. manualOffsets on a label drag — which fires the
  // reset-to-fit effect below and wipes the magnifier zoom/pan.
  const geo = useMemo(
    () => exportGeometry(size),
    [size.dpi, size.fontPt, size.labelFontPt, size.axisWidthPt, size.widthMm, size.heightMm, size.pointRadiusMm],
  );
  const ranges = useMemo(() => resolveRanges(size.xRange, size.yRange, series), [size.xRange, size.yRange, series]);
  const index = useMemo<HitIndex>(() => buildHitIndex(series), [series]);
  const anchorMap = useMemo(() => {
    const m = new Map<string, { x: number; y: number }>();
    for (const p of labelPoints) m.set(p.gene, { x: p.x, y: p.y });
    return m;
  }, [labelPoints]);

  // Latest inputs via refs for the imperative handlers.
  const R = useRef({ series, geo, ranges, index, anchorMap, labelPoints, manualOffsets, shapes, xTitle, yTitle, hoverFormat, onManualMove, onToggleLabel, onHover, selectedGene, onSelectGene });
  R.current = { series, geo, ranges, index, anchorMap, labelPoints, manualOffsets, shapes, xTitle, yTitle, hoverFormat, onManualMove, onToggleLabel, onHover, selectedGene, onSelectGene };

  // Magnifier view (CSS px): figure top-left = (offX, offY), drawn at fitScale*mag.
  const viewRef = useRef({ mag: 1, offX: 0, offY: 0 });
  const dragRef = useRef<{ mode: "label" | "pan"; sx: number; sy: number; offX: number; offY: number; gene?: string; grabX?: number; grabY?: number; moved: boolean; dotGene?: string } | null>(null);
  const overrideRef = useRef<{ gene: string; off: ManualOffset } | null>(null);
  // Drawn-frame state for hit-testing.
  const vs = useRef({ fitScale: 1, drawScale: 1, offX: 0, offY: 0, placements: [] as LabelPlacement[] });

  // Offscreen dots layer: the expensive scatter is rendered once per data change
  // (in viewBox units, supersampled) and blitted each frame, so pan/zoom/label
  // drag stay fast regardless of point count.
  const DOT_SS = 2;
  const dotsCanvasRef = useRef<HTMLCanvasElement | null>(null);
  const dotsDepsRef = useRef<{ series?: Series[]; geo?: typeof geo; ranges?: typeof ranges }>({});
  const ensureDots = useCallback(() => {
    const g = R.current.geo;
    const rng = R.current.ranges;
    const ser = R.current.series;
    const d = dotsDepsRef.current;
    if (dotsCanvasRef.current && d.series === ser && d.geo === g && d.ranges === rng) return dotsCanvasRef.current;
    const cv = dotsCanvasRef.current ?? document.createElement("canvas");
    cv.width = Math.max(1, Math.round(g.vbW * DOT_SS));
    cv.height = Math.max(1, Math.round(g.vbH * DOT_SS));
    const c = cv.getContext("2d");
    if (c) {
      c.setTransform(DOT_SS, 0, 0, DOT_SS, 0, 0);
      c.clearRect(0, 0, g.vbW, g.vbH);
      c.save();
      c.beginPath();
      c.rect(g.m.l, g.m.t, g.plotW, g.plotH);
      c.clip();
      const r = g.markerDiaPx / 2;
      const dx = rng.xr[1] - rng.xr[0] || 1;
      const dy = rng.yr[1] - rng.yr[0] || 1;
      for (const sv of ser) {
        c.fillStyle = sv.color;
        const n = Math.min(sv.x.length, sv.y.length);
        for (let i = 0; i < n; i++) {
          const px = g.m.l + ((sv.x[i] - rng.xr[0]) / dx) * g.plotW;
          const py = g.m.t + (1 - (sv.y[i] - rng.yr[0]) / dy) * g.plotH;
          c.beginPath();
          c.arc(px, py, r, 0, 2 * Math.PI);
          c.fill();
        }
      }
      c.restore();
    }
    dotsCanvasRef.current = cv;
    dotsDepsRef.current = { series: ser, geo: g, ranges: rng };
    return cv;
  }, []);

  const fitScaleFor = useCallback((boxW: number, boxH: number) => {
    const pad = 16;
    return Math.min((boxW - pad) / R.current.geo.vbW, (boxH - pad) / R.current.geo.vbH) || 1;
  }, []);

  const resetFit = useCallback(() => {
    const box = boxRef.current;
    if (!box) return;
    const fs = fitScaleFor(box.clientWidth, box.clientHeight);
    const g = R.current.geo;
    viewRef.current = {
      mag: 1,
      offX: (box.clientWidth - g.vbW * fs) / 2,
      offY: (box.clientHeight - g.vbH * fs) / 2,
    };
  }, [fitScaleFor]);

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
    const { geo: g, ranges: rng, series: ser, labelPoints: lp, manualOffsets: mo, shapes: sh, xTitle: xt, yTitle: yt } = R.current;
    const v = viewRef.current;
    const fitScale = fitScaleFor(cw, ch);
    const drawScale = fitScale * v.mag;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cw, ch);

    const rect = { left: g.m.l, top: g.m.t, width: g.plotW, height: g.plotH };
    const eff = overrideRef.current ? { ...mo, [overrideRef.current.gene]: overrideRef.current.off } : mo;
    const placements = layoutLabels(lp, rect, rng.xr, rng.yr, g.labelFontPx, eff);
    vs.current = { fitScale, drawScale, offX: v.offX, offY: v.offY, placements };

    ctx.save();
    ctx.translate(v.offX, v.offY);
    ctx.scale(drawScale, drawScale);
    // White page so the magnifier shows the figure bounds clearly.
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, g.vbW, g.vbH);

    // Below the cache's supersample density, blitting the cached dots is crisp
    // and fast. Once magnified beyond it, draw the (few) VISIBLE dots directly
    // as vectors so they stay sharp — viewport culling keeps it cheap.
    const crisp = !interactingRef.current && drawScale * dpr > DOT_SS + 0.01;
    let viewport: { left: number; top: number; right: number; bottom: number } | undefined;
    if (crisp) {
      viewport = {
        left: (0 - v.offX) / drawScale,
        top: (0 - v.offY) / drawScale,
        right: (cw - v.offX) / drawScale,
        bottom: (ch - v.offY) / drawScale,
      };
    } else {
      const dots = ensureDots();
      ctx.save();
      ctx.beginPath();
      ctx.rect(g.m.l, g.m.t, g.plotW, g.plotH);
      ctx.clip();
      ctx.drawImage(dots, 0, 0, g.vbW, g.vbH);
      ctx.restore();
    }
    drawFigure(ctx, {
      series: ser,
      xr: rng.xr,
      yr: rng.yr,
      rect,
      fontPx: g.fontPx,
      labelFontPx: g.labelFontPx,
      axisWidthPx: g.axisWidthPx,
      markerRadiusPx: g.markerDiaPx / 2,
      xTitle: xt,
      yTitle: yt,
      shapes: sh,
      labels: lp,
      placements,
      manualOffsets: eff,
      skipDots: !crisp,
      viewport,
      xDtick: size.xDtick,
      yDtick: size.yDtick,
      selectedGene: R.current.selectedGene,
    });
    ctx.restore();
    if (tipRef.current) tipRef.current.dataset.pct = String(Math.round(v.mag * 100));
  }, [fitScaleFor, ensureDots, size.xDtick, size.yDtick]);

  const scheduleDraw = useCallback(() => {
    if (rafRef.current !== undefined) return;
    rafRef.current = requestAnimationFrame(() => {
      rafRef.current = undefined;
      draw();
    });
  }, [draw]);

  // During a gesture, draw the cheap cached-dots blit; once it settles, redraw
  // with crisp vector dots. Keeps pan/zoom smooth regardless of point count.
  const interactingRef = useRef(false);
  const settleRef = useRef<number | undefined>(undefined);
  const interact = useCallback(() => {
    interactingRef.current = true;
    scheduleDraw();
    if (settleRef.current !== undefined) window.clearTimeout(settleRef.current);
    settleRef.current = window.setTimeout(() => {
      interactingRef.current = false;
      scheduleDraw();
    }, 160);
  }, [scheduleDraw]);
  useEffect(() => () => { if (settleRef.current !== undefined) window.clearTimeout(settleRef.current); }, []);

  // Reset to fit on geometry change / mount / Fit button (fitTick).
  useEffect(() => {
    resetFit();
    scheduleDraw();
  }, [geo, fitTick, resetFit, scheduleDraw]);

  // Redraw on content change.
  useEffect(() => { scheduleDraw(); }, [series, labelPoints, manualOffsets, shapes, xTitle, yTitle, ranges, selectedGene, scheduleDraw]);

  useEffect(() => {
    const box = boxRef.current;
    if (!box) return;
    const ro = new ResizeObserver(() => scheduleDraw());
    ro.observe(box);
    return () => ro.disconnect();
  }, [scheduleDraw]);

  // --- coordinate helpers (screen px within box) ---
  const screenToVb = (sx: number, sy: number) => ({
    x: (sx - vs.current.offX) / vs.current.drawScale,
    y: (sy - vs.current.offY) / vs.current.drawScale,
  });
  const vbToData = (vx: number, vy: number) => {
    const g = R.current.geo;
    const { xr, yr } = R.current.ranges;
    return {
      x: xr[0] + ((vx - g.m.l) / g.plotW) * (xr[1] - xr[0]),
      y: yr[0] + (1 - (vy - g.m.t) / g.plotH) * (yr[1] - yr[0]),
    };
  };

  const pickLabel = (sx: number, sy: number): LabelPlacement | null => {
    const vb = screenToVb(sx, sy);
    for (const p of vs.current.placements) {
      if (Math.abs(vb.x - p.center.x) <= p.w / 2 && Math.abs(vb.y - p.center.y) <= p.h / 2) return p;
    }
    return null;
  };
  const pickDot = (sx: number, sy: number) => {
    const vb = screenToVb(sx, sy);
    const data = vbToData(vb.x, vb.y);
    const g = R.current.geo;
    const { xr, yr } = R.current.ranges;
    const idx = R.current.index;
    const ser = R.current.series;
    const thrVb = HIT_PX / vs.current.drawScale;
    // threshold in data space (per axis) from a vb threshold
    const thrX = (thrVb / g.plotW) * (xr[1] - xr[0]);
    const thrY = (thrVb / g.plotH) * (yr[1] - yr[0]);
    const c0 = Math.max(0, Math.floor((data.x - thrX - idx.minX) / idx.cellW));
    const c1 = Math.min(idx.cols - 1, Math.floor((data.x + thrX - idx.minX) / idx.cellW));
    const r0 = Math.max(0, Math.floor((data.y - thrY - idx.minY) / idx.cellH));
    const r1 = Math.min(idx.rows - 1, Math.floor((data.y + thrY - idx.minY) / idx.cellH));
    let best: { si: number; pi: number } | null = null;
    let bestD = thrVb * thrVb;
    for (let cy = r0; cy <= r1; cy++) {
      for (let cx = c0; cx <= c1; cx++) {
        const cell = idx.cells[cy * idx.cols + cx];
        if (!cell) continue;
        for (let k = 0; k < cell.length; k += 2) {
          const si = cell[k];
          const pi = cell[k + 1];
          const s = ser[si];
          const dvx = g.m.l + ((s.x[pi] - xr[0]) / (xr[1] - xr[0])) * g.plotW;
          const dvy = g.m.t + (1 - (s.y[pi] - yr[0]) / (yr[1] - yr[0])) * g.plotH;
          const d = (dvx - vb.x) * (dvx - vb.x) + (dvy - vb.y) * (dvy - vb.y);
          if (d <= bestD) { bestD = d; best = { si, pi }; }
        }
      }
    }
    return best ? { gene: ser[best.si].ids[best.pi], x: ser[best.si].x[best.pi], y: ser[best.si].y[best.pi] } : null;
  };

  const localXY = (e: React.PointerEvent | WheelEvent) => {
    const r = boxRef.current!.getBoundingClientRect();
    return { sx: e.clientX - r.left, sy: e.clientY - r.top };
  };

  const onPointerDown = (e: React.PointerEvent) => {
    const { sx, sy } = localXY(e);
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    const lab = pickLabel(sx, sy);
    if (lab) {
      const vb = screenToVb(sx, sy);
      dragRef.current = { mode: "label", sx, sy, offX: 0, offY: 0, gene: lab.gene, grabX: lab.center.x - vb.x, grabY: lab.center.y - vb.y, moved: false };
      return;
    }
    const dot = pickDot(sx, sy);
    dragRef.current = { mode: "pan", sx, sy, offX: viewRef.current.offX, offY: viewRef.current.offY, moved: false, dotGene: dot?.gene };
  };

  const onPointerMove = (e: React.PointerEvent) => {
    const { sx, sy } = localXY(e);
    const d = dragRef.current;
    if (!d) {
      // hover
      const dot = pickDot(sx, sy);
      const tip = tipRef.current;
      R.current.onHover?.(dot ? dot.gene : null);
      if (dot && tip) {
        tip.style.display = "block";
        tip.style.left = `${sx + 12}px`;
        tip.style.top = `${sy + 12}px`;
        tip.textContent = R.current.hoverFormat(dot.gene, dot.x, dot.y);
      } else if (tip) {
        tip.style.display = "none";
      }
      return;
    }
    if (Math.abs(sx - d.sx) + Math.abs(sy - d.sy) > 3) d.moved = true;
    if (d.mode === "label" && d.gene) {
      const vb = screenToVb(sx, sy);
      const cx = vb.x + (d.grabX ?? 0);
      const cy = vb.y + (d.grabY ?? 0);
      const cd = vbToData(cx, cy);
      const a = R.current.anchorMap.get(d.gene);
      if (a) overrideRef.current = { gene: d.gene, off: { dx: cd.x - a.x, dy: cd.y - a.y } };
      interact();
    } else if (d.mode === "pan") {
      viewRef.current = { ...viewRef.current, offX: d.offX + (sx - d.sx), offY: d.offY + (sy - d.sy) };
      interact();
    }
  };

  const onPointerUp = () => {
    const d = dragRef.current;
    dragRef.current = null;
    if (!d) return;
    if (d.mode === "label") {
      if (overrideRef.current) {
        // Dragged → commit the manual position.
        const o = overrideRef.current;
        overrideRef.current = null;
        R.current.onManualMove(o.gene, o.off);
      } else if (d.gene) {
        // Clicked (no drag) → select the gene.
        R.current.onSelectGene?.(d.gene);
      }
    } else if (d.mode === "pan" && !d.moved) {
      if (d.dotGene) R.current.onToggleLabel(d.dotGene); // click dot → toggle label
      else R.current.onSelectGene?.(null); // click empty → clear selection
    }
  };

  const onPointerLeave = () => {
    if (tipRef.current) tipRef.current.style.display = "none";
    R.current.onHover?.(null);
  };

  // Wheel zoom toward the cursor.
  useEffect(() => {
    const box = boxRef.current;
    if (!box) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const { sx, sy } = localXY(e);
      const v = viewRef.current;
      // Match the interactive plot's wheel feel (fixed step; up = zoom in).
      const factor = e.deltaY < 0 ? 1.15 : 1 / 1.15;
      const nm = Math.min(40, Math.max(0.1, v.mag * factor));
      const fs = vs.current.fitScale;
      const oldScale = fs * v.mag;
      const newScale = fs * nm;
      // keep the figure point under the cursor fixed
      const vbx = (sx - v.offX) / oldScale;
      const vby = (sy - v.offY) / oldScale;
      viewRef.current = { mag: nm, offX: sx - vbx * newScale, offY: sy - vby * newScale };
      interact();
    };
    box.addEventListener("wheel", onWheel, { passive: false });
    return () => box.removeEventListener("wheel", onWheel);
  }, [interact]);

  return (
    <div
      ref={boxRef}
      style={{ position: "relative", width: "100%", height: "100%", minHeight: 0, overflow: "hidden", background: "#e9e9e9", cursor: "crosshair", touchAction: "none" }}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerLeave={onPointerLeave}
    >
      <canvas ref={canvasRef} style={{ display: "block" }} />
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
