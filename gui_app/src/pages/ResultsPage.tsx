// Visualization-only mode. See gui_plan.md § 5.3 and § 8.
//
// Loads a results.csv via Tauri, renders volcano + MA + table, with shared
// selection state so clicking a gene in any panel highlights it in all.

import { useCallback, useEffect, useMemo, useRef, useState, type CSSProperties } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import { copySvg, loadResultsCsv, savePlot } from "../lib/tauri";
import {
  classifyRow,
  countSignificance,
  type ResultsRow,
  type ResultsTable as ResultsTableData,
} from "../lib/results";
import { useAppState } from "../lib/project_context";
import { Volcano } from "../components/plots/Volcano";
import { MAPlot } from "../components/plots/MAPlot";
import { ExportPreview } from "../components/plots/ExportPreview";
import { buildPanelFigure, panelLabelPoints } from "../components/plots/panel_figure";
import { ResultsTable, type SortKey } from "../components/ResultsTable";
import {
  detectSpecies,
  geneDisplayName,
  loadGeneSymbols,
  loadCustomGeneMap,
  stripVersion,
  type GeneNameMode,
  type Species,
} from "../lib/gene_symbols";
import type {
  ExportFn,
  ExportFormat,
  ExportSize,
  ManualOffset,
} from "../components/plots/types";

type Panel = "volcano" | "ma";

// Resolved gene-symbol map source. The species is auto-detected; a 'file'
// source is a user-supplied override (human/fly) or the custom map for IDs that
// are neither ENSG nor FBgn.
type MapSource =
  | { kind: "species"; species: Species }
  | { kind: "file"; path: string; label: string };

const PANELS: { id: Panel; label: string }[] = [
  { id: "volcano", label: "Volcano" },
  { id: "ma", label: "MA" },
];

const RESULTS_FILTER = [
  { name: "Results CSV", extensions: ["csv", "tsv", "txt"] },
];

// Publication-export settings, kept INDEPENDENT per panel (volcano vs MA).
interface PanelSettings {
  fontSize: number;
  labelFontSize: number;
  widthMm: number;
  heightMm: number;
  dpi: number;
  pointRadiusMm: number;
  axisWidthPt: number;
  xMin: string;
  xMax: string;
  yMin: string;
  yMax: string;
  xTick: string;
  yTick: string;
  manualOffsets: Record<string, ManualOffset>;
}
const DEFAULT_SETTINGS: PanelSettings = {
  fontSize: 7, labelFontSize: 5, widthMm: 50, heightMm: 50, dpi: 300, pointRadiusMm: 0.3, axisWidthPt: 0.5,
  xMin: "", xMax: "", yMin: "", yMax: "", xTick: "", yTick: "", manualOffsets: {},
};

const SETTING_ROW: CSSProperties = { display: "flex", alignItems: "center", gap: 6 };
const SETTING_VAL: CSSProperties = { marginLeft: "auto", display: "flex", alignItems: "center", gap: 4 };

const NO_ROWS: ResultsRow[] = []; // stable empty ref for "no table loaded"

export function ResultsPage() {
  const [table, setTable] = useState<ResultsTableData | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState<boolean>(false);
  const [activePanel, setActivePanel] = useState<Panel>("volcano");
  const [padjCutoff, setPadjCutoff] = useState<number>(0.05);
  const [log2fcCutoff, setLog2fcCutoff] = useState<number>(1.0);
  const [search, setSearch] = useState<string>("");
  // Table-only filter: show just the DEGs (genes passing the padj + |log2FC|
  // cutoffs). The plot still shows every point.
  const [degOnly, setDegOnly] = useState<boolean>(false);
  const [sortKey, setSortKey] = useState<SortKey>("padj");
  const [sortDir, setSortDir] = useState<"asc" | "desc">("asc");
  const [hoveredGene, setHoveredGene] = useState<string | null>(null);
  const [renderMs, setRenderMs] = useState<number | null>(null);
  const [canExport, setCanExport] = useState(false);
  const [savedMsg, setSavedMsg] = useState<string | null>(null);

  // App-level settings (Preferences). The gene-map preference decides which
  // symbol map the Results view uses; 'auto' detects the species from gene IDs.
  const { pendingResultsPath, setPendingResultsPath, geneMapPaths } = useAppState();

  // Genes checked for on-plot labeling.
  const [labeledGenes, setLabeledGenes] = useState<Set<string>>(new Set());
  // Gene selected by clicking its on-plot label: highlighted on the plot,
  // scrolled-to in the table, and removable with Delete.
  const [selectedGene, setSelectedGene] = useState<string | null>(null);
  // ENSG <-> gene-symbol display toggle (display only; identity stays gene_id).
  const [geneNameMode, setGeneNameMode] = useState<GeneNameMode>("ensembl");
  const [symbolMap, setSymbolMap] = useState<Map<string, string> | null>(null);
  const [symbolError, setSymbolError] = useState<string | null>(null);
  // Species detected from the gene_id format (ENSG -> human, FBgn -> fly) for
  // the 'auto' preference. null = ids aren't a recognised format.
  const autoSpecies = useMemo<Species | null>(
    () => (table ? detectSpecies(table.rows.map((r) => r.gene_id)) : null),
    [table],
  );
  // Resolve which map to use: auto-detected species, with optional user files.
  // human/fly → user override file when set, else the bundled map; otherwise
  // (IDs not ENSG/FBgn) → the custom map when provided.
  const mapSource = useMemo<MapSource | null>(() => {
    if (autoSpecies === "human")
      return geneMapPaths.human ? { kind: "file", path: geneMapPaths.human, label: "human" } : { kind: "species", species: "human" };
    if (autoSpecies === "fly")
      return geneMapPaths.fly ? { kind: "file", path: geneMapPaths.fly, label: "fly" } : { kind: "species", species: "fly" };
    return geneMapPaths.custom ? { kind: "file", path: geneMapPaths.custom, label: "custom" } : null;
  }, [autoSpecies, geneMapPaths]);
  // Stable key + label for the resolved source.
  const mapKey = mapSource ? (mapSource.kind === "file" ? `file:${mapSource.path}` : mapSource.species) : null;
  const sourceLabel = mapSource ? (mapSource.kind === "file" ? mapSource.label : mapSource.species) : null;
  // A loaded map belongs to one source; drop it when the source changes.
  useEffect(() => { setSymbolMap(null); setSymbolError(null); }, [mapKey]);
  // Load the map lazily — the first time symbols are shown OR the user searches
  // (so search-by-symbol works even in id display mode).
  useEffect(() => {
    const needed = geneNameMode === "symbol" || search.trim().length > 0;
    if (!needed || !mapSource || symbolMap) return;
    let cancelled = false;
    const loader = mapSource.kind === "file"
      ? loadCustomGeneMap(mapSource.path)
      : loadGeneSymbols(mapSource.species);
    loader
      .then((m) => { if (!cancelled) { setSymbolMap(m); setSymbolError(null); } })
      .catch((e) => { if (!cancelled) setSymbolError(e instanceof Error ? e.message : String(e)); });
    return () => { cancelled = true; };
  }, [geneNameMode, search, symbolMap, mapSource]);
  const displayName = useCallback(
    (geneId: string) => geneDisplayName(geneId, symbolMap, geneNameMode),
    [symbolMap, geneNameMode],
  );
  // Publication settings popover (opened from the Export preview header).
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [settingsByPanel, setSettingsByPanel] = useState<Record<Panel, PanelSettings>>({
    volcano: { ...DEFAULT_SETTINGS },
    ma: { ...DEFAULT_SETTINGS },
  });
  const s = settingsByPanel[activePanel];
  const { fontSize, labelFontSize, widthMm, heightMm, dpi, pointRadiusMm, axisWidthPt, xMin, xMax, yMin, yMax, xTick, yTick, manualOffsets } = s;
  const patchSettings = useCallback(
    (p: Partial<PanelSettings>) =>
      setSettingsByPanel((prev) => ({ ...prev, [activePanel]: { ...prev[activePanel], ...p } })),
    [activePanel],
  );
  const setFontSize = (v: number) => patchSettings({ fontSize: v });
  const setLabelFontSize = (v: number) => patchSettings({ labelFontSize: v });
  const setWidthMm = (v: number) => patchSettings({ widthMm: v });
  const setHeightMm = (v: number) => patchSettings({ heightMm: v });
  const setDpi = (v: number) => patchSettings({ dpi: v });
  const setPointRadiusMm = (v: number) => patchSettings({ pointRadiusMm: v });
  const setAxisWidthPt = (v: number) => patchSettings({ axisWidthPt: v });
  const setXMin = (v: string) => patchSettings({ xMin: v });
  const setXMax = (v: string) => patchSettings({ xMax: v });
  const setYMin = (v: string) => patchSettings({ yMin: v });
  const setYMax = (v: string) => patchSettings({ yMax: v });
  const setXTick = (v: string) => patchSettings({ xTick: v });
  const setYTick = (v: string) => patchSettings({ yTick: v });
  const setManualOffsets = useCallback(
    (u: Record<string, ManualOffset> | ((p: Record<string, ManualOffset>) => Record<string, ManualOffset>)) =>
      setSettingsByPanel((prev) => {
        const cur = prev[activePanel];
        const next = typeof u === "function" ? u(cur.manualOffsets) : u;
        return { ...prev, [activePanel]: { ...cur, manualOffsets: next } };
      }),
    [activePanel],
  );
  // Canvas export preview (live; the figure is drawn with the export engine).
  const [previewOpen, setPreviewOpen] = useState(false);
  const [fitTick, setFitTick] = useState(0); // bump to fit the preview to window
  const [copied, setCopied] = useState(false); // brief Copy-SVG button feedback
  const copiedTimer = useRef<number | undefined>(undefined);

  // Manual label placement (drag in the preview → applies to export).
  const onManualMove = useCallback((gene: string, off: ManualOffset) => {
    setManualOffsets((prev) => ({ ...prev, [gene]: off }));
  }, [setManualOffsets]);

  const exportRef = useRef<ExportFn | null>(null);

  const toggleLabel = useCallback((gene: string) => {
    setLabeledGenes((prev) => {
      const next = new Set(prev);
      if (next.has(gene)) next.delete(gene);
      else next.add(gene);
      return next;
    });
  }, []);
  const clearLabels = useCallback(() => setLabeledGenes(new Set()), []);

  // A selection only makes sense while the gene is labeled; drop it otherwise.
  useEffect(() => {
    if (selectedGene && !labeledGenes.has(selectedGene)) setSelectedGene(null);
  }, [labeledGenes, selectedGene]);

  // Delete / Backspace removes the selected gene's label (unless typing in a field).
  useEffect(() => {
    if (!selectedGene) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Delete" && e.key !== "Backspace") return;
      const t = e.target as HTMLElement | null;
      const tag = t?.tagName;
      if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || t?.isContentEditable) return;
      e.preventDefault();
      const gene = selectedGene;
      setLabeledGenes((prev) => { const n = new Set(prev); n.delete(gene); return n; });
      setSelectedGene(null);
    };
    document.addEventListener("keydown", onKey);
    return () => document.removeEventListener("keydown", onKey);
  }, [selectedGene]);

  const parseRange = (lo: string, hi: string): [number, number] | null => {
    const a = Number(lo);
    const b = Number(hi);
    if (lo.trim() === "" || hi.trim() === "" || !Number.isFinite(a) || !Number.isFinite(b) || a === b) {
      return null;
    }
    return [Math.min(a, b), Math.max(a, b)];
  };

  const pub = useMemo(
    () => ({
      fontSize,
      xRange: parseRange(xMin, xMax),
      yRange: parseRange(yMin, yMax),
      labeledGenes,
      manualOffsets,
      revision: 0,
    }),
    [fontSize, xMin, xMax, yMin, yMax, labeledGenes, manualOffsets],
  );

  const buildExportSize = useCallback((): ExportSize => {
    // Full-resolution px viewBox (mm * dpi) so dots/raster are crisp; the SVG
    // root is then given the physical mm size. Fonts are converted to px at
    // this dpi inside the plot.
    const mm2px = (mm: number) => Math.max(1, Math.round((mm / 25.4) * dpi));
    return {
      width: mm2px(widthMm),
      height: mm2px(heightMm),
      widthMm,
      heightMm,
      dpi,
      fontPt: fontSize,
      labelFontPt: labelFontSize,
      pointRadiusMm,
      axisWidthPt,
      xDtick: Number(xTick) || undefined,
      yDtick: Number(yTick) || undefined,
      xRange: parseRange(xMin, xMax),
      yRange: parseRange(yMin, yMax),
      manualOffsets,
    };
  }, [widthMm, heightMm, dpi, fontSize, labelFontSize, pointRadiusMm, axisWidthPt, xTick, yTick, xMin, xMax, yMin, yMax, manualOffsets]);

  const exportSize = useMemo(() => buildExportSize(), [buildExportSize]);


  const registerExport = useCallback((fn: ExportFn | null) => {
    exportRef.current = fn;
    setCanExport(fn !== null);
  }, []);

  // Auto-load a results path produced by a successful Run.
  useEffect(() => {
    if (!pendingResultsPath) return;
    let cancelled = false;
    (async () => {
      setLoading(true);
      setError(null);
      try {
        const loaded = await loadResultsCsv(pendingResultsPath);
        if (cancelled) return;
        setTable(loaded);
        setRenderMs(null);
        setSavedMsg(null);
        setLabeledGenes(new Set());
        setPreviewOpen(false);
      } catch (e) {
        if (!cancelled) setError(formatError(e));
      } finally {
        if (!cancelled) {
          setLoading(false);
          setPendingResultsPath(null);
        }
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [pendingResultsPath, setPendingResultsPath]);

  const handleExport = useCallback(
    async (format: ExportFormat) => {
      const fn = exportRef.current;
      if (!fn || !table) return;
      setError(null);
      try {
        const picked = await save({
          defaultPath: `${activePanel}.${format}`,
          filters: [{ name: format.toUpperCase(), extensions: [format] }],
        });
        if (!picked) return;
        const bytes = await fn(format, buildExportSize());
        await savePlot(picked, bytes);
        setSavedMsg(`Saved ${picked} (${widthMm}×${heightMm} mm @ ${dpi} dpi)`);
      } catch (e) {
        setError(formatError(e));
      }
    },
    [table, activePanel, buildExportSize, widthMm, heightMm, dpi],
  );

  // Copy the figure to the clipboard as a vector SVG (image/svg+xml, written
  // natively by the Rust backend) so PowerPoint / Word paste it as a vector
  // graphic. (Windows; the async Clipboard API can't write this format.)
  const handleCopySvg = useCallback(async () => {
    const fn = exportRef.current;
    if (!fn) return;
    setError(null);
    try {
      const bytes = await fn("svg", buildExportSize());
      await copySvg(new TextDecoder().decode(bytes as Uint8Array));
      setCopied(true);
      if (copiedTimer.current !== undefined) window.clearTimeout(copiedTimer.current);
      copiedTimer.current = window.setTimeout(() => setCopied(false), 1200);
    } catch (e) {
      setError(formatError(e));
    }
  }, [buildExportSize]);

  useEffect(() => () => { if (copiedTimer.current !== undefined) window.clearTimeout(copiedTimer.current); }, []);

  async function handleOpen() {
    setError(null);
    try {
      const picked = await open({
        multiple: false,
        directory: false,
        filters: RESULTS_FILTER,
      });
      if (!picked) return;
      const path = Array.isArray(picked) ? picked[0] : picked;
      setLoading(true);
      const loaded = await loadResultsCsv(path);
      setTable(loaded);
      setRenderMs(null);
      setSavedMsg(null);
      setLabeledGenes(new Set());
      setPreviewOpen(false);
    } catch (e) {
      setError(formatError(e));
    } finally {
      setLoading(false);
    }
  }

  function handleSort(key: SortKey) {
    if (key === sortKey) {
      setSortDir((d) => (d === "asc" ? "desc" : "asc"));
    } else {
      setSortKey(key);
      setSortDir(key === "gene_id" ? "asc" : key === "log2FoldChange" ? "desc" : "asc");
    }
  }

  // The plot always shows every point; search must NOT strip the plot.
  const allRows = table?.rows ?? NO_ROWS;

  // Table-only filters (the plot always shows every point): optionally keep
  // only DEGs (passing the padj + |log2FC| cutoffs), then match the typed text
  // against gene_id OR gene symbol (symbol works regardless of display mode,
  // once the map has loaded).
  const filteredRows = useMemo<ResultsRow[]>(() => {
    let rows = allRows;
    if (degOnly) {
      rows = rows.filter((r) => {
        const c = classifyRow(r, padjCutoff, log2fcCutoff);
        return c === "up" || c === "down";
      });
    }
    const needle = search.trim().toLowerCase();
    if (needle) {
      rows = rows.filter((r) => {
        if (r.gene_id.toLowerCase().includes(needle)) return true;
        const sym = symbolMap?.get(r.gene_id) ?? symbolMap?.get(stripVersion(r.gene_id));
        return !!sym && sym.toLowerCase().includes(needle);
      });
    }
    return rows;
  }, [allRows, search, symbolMap, degOnly, padjCutoff, log2fcCutoff]);

  // Significance counts summarise the whole dataset, not the table search.
  const counts = useMemo(
    () => (table ? countSignificance(allRows, padjCutoff, log2fcCutoff) : null),
    [table, allRows, padjCutoff, log2fcCutoff],
  );

  // Figure for the live canvas export preview (active panel).
  const previewFig = useMemo(
    () => buildPanelFigure(activePanel, allRows, padjCutoff, log2fcCutoff),
    [activePanel, allRows, padjCutoff, log2fcCutoff],
  );
  const previewLabelPoints = useMemo(
    () => panelLabelPoints(activePanel, allRows, labeledGenes, displayName),
    [activePanel, allRows, labeledGenes, displayName],
  );

  return (
    <div style={{ padding: 12, display: "flex", flexDirection: "column", gap: 10, height: "100%" }}>
      <header style={{ display: "flex", alignItems: "center", gap: 12, flexWrap: "wrap" }}>
        <button onClick={handleOpen} disabled={loading}>
          {loading ? "Loading…" : "Open results.csv…"}
        </button>
        {table && (
          <span style={{ fontSize: 12, opacity: 0.75 }}>
            <code>{table.source_path}</code>
            {" · "}
            {table.n_total.toLocaleString()} rows
            {" · "}
            {table.n_with_padj.toLocaleString()} with padj
            {table.source_tool && ` · tool=${table.source_tool}`}
          </span>
        )}
        <div style={{ flex: 1 }} />
      </header>

      {savedMsg && (
        <div style={{ fontSize: 12, color: "rgb(20,100,20)", wordBreak: "break-all" }}>
          {savedMsg}
        </div>
      )}

      {error && (
        <div style={{ color: "#b00020", fontSize: 12, whiteSpace: "pre-wrap" }}>
          {error}
        </div>
      )}

      {!table && !loading && (
        <div style={{ padding: 24, opacity: 0.6, fontSize: 13 }}>
          Open a results.csv produced by FlashDEG, DESeq2 R, PyDESeq2, or
          InMoose. Required columns: <code>gene_id, baseMean, log2FoldChange, lfcSE, stat, pvalue, padj</code>.
        </div>
      )}

      {table && (
        <>
          <FiltersBar
            padjCutoff={padjCutoff}
            log2fcCutoff={log2fcCutoff}
            search={search}
            degOnly={degOnly}
            symbolMode={geneNameMode === "symbol"}
            sourceLabel={sourceLabel}
            symbolNote={
              symbolError
                ? `Gene symbols unavailable: ${symbolError}`
                : geneNameMode === "symbol" && !mapSource
                  ? "Gene IDs aren't ENSG/FBgn — add a Custom gene map in Preferences (Ctrl/Cmd+,)."
                  : null
            }
            labelCount={labeledGenes.size}
            onPadjChange={setPadjCutoff}
            onLog2fcChange={setLog2fcCutoff}
            onSearchChange={setSearch}
            onDegOnlyChange={setDegOnly}
            onSymbolModeChange={(v) => setGeneNameMode(v ? "symbol" : "ensembl")}
            onClearLabels={clearLabels}
          />

          <SummaryBar
            counts={counts}
            shown={filteredRows.length}
            total={table.n_total}
            hoveredGene={hoveredGene}
            renderMs={renderMs}
          />

          <div style={{ flex: 1, minHeight: 0, display: "flex", gap: 12 }}>
            <div
              style={{
                flex: "1 1 60%",
                minWidth: 0,
                border: "1px solid rgba(127,127,127,0.25)",
                borderRadius: 6,
                overflow: "hidden",
                display: "flex",
                flexDirection: "column",
              }}
            >
              {/* Tab row at the top of the graph panel: Volcano/MA (left) and the
                  Interactive/Publication switch (right, at the graph's edge). */}
              <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", gap: 6, padding: "4px 6px", borderBottom: "1px solid rgba(127,127,127,0.2)" }}>
                <div style={{ display: "flex" }} role="group" aria-label="Plot">
                  {PANELS.map((p, i) => {
                    const isLeft = i === 0;
                    const active = activePanel === p.id;
                    const self = active ? "#0072B2" : "rgba(127,127,127,0.45)";
                    return (
                      <button
                        key={p.id}
                        onClick={() => { setActivePanel(p.id); setRenderMs(null); setSavedMsg(null); }}
                        style={{
                          boxSizing: "border-box",
                          borderTop: `1px solid ${self}`,
                          borderBottom: `1px solid ${self}`,
                          borderLeft: isLeft ? `1px solid ${self}` : "none",
                          borderRight: isLeft ? "1px solid #0072B2" : `1px solid ${self}`,
                          borderTopLeftRadius: isLeft ? 6 : 0,
                          borderBottomLeftRadius: isLeft ? 6 : 0,
                          borderTopRightRadius: isLeft ? 0 : 6,
                          borderBottomRightRadius: isLeft ? 0 : 6,
                          padding: "3px 12px",
                          fontSize: 12,
                          fontWeight: active ? 600 : 400,
                          background: active ? "rgba(0,114,178,0.12)" : "transparent",
                          color: active ? "#0072B2" : "#888",
                          cursor: "pointer",
                        }}
                      >
                        {p.label}
                      </button>
                    );
                  })}
                </div>
                <div role="group" aria-label="Mode" style={{ display: "flex" }}>
                {(["interactive", "publication"] as const).map((mode) => {
                  const isLeft = mode === "interactive";
                  const active = (mode === "publication") === previewOpen;
                  const disabled = mode === "publication" && !table;
                  const self = active ? "#0072B2" : "rgba(127,127,127,0.45)";
                  return (
                    <button
                      key={mode}
                      onClick={() => !disabled && setPreviewOpen(mode === "publication")}
                      disabled={disabled}
                      title={mode === "publication" ? "Publication export preview" : "Interactive plot"}
                      style={{
                        boxSizing: "border-box",
                        borderTop: `1px solid ${self}`,
                        borderBottom: `1px solid ${self}`,
                        borderLeft: isLeft ? `1px solid ${self}` : "none",
                        borderRight: isLeft ? "1px solid #0072B2" : `1px solid ${self}`,
                        borderTopLeftRadius: isLeft ? 6 : 0,
                        borderBottomLeftRadius: isLeft ? 6 : 0,
                        borderTopRightRadius: isLeft ? 0 : 6,
                        borderBottomRightRadius: isLeft ? 0 : 6,
                        padding: "3px 12px",
                        fontSize: 12,
                        fontWeight: active ? 600 : 400,
                        background: active ? "rgba(0,114,178,0.12)" : "transparent",
                        color: active ? "#0072B2" : disabled ? "#bbb" : "#888",
                        cursor: disabled ? "not-allowed" : "pointer",
                      }}
                    >
                      {isLeft ? "Interactive" : "Publication"}
                    </button>
                  );
                })}
                </div>
              </div>
              <div style={{ flex: 1, minHeight: 0, position: "relative" }}>
              {/* Plot stays mounted (so its export fn stays registered); the
                  preview overlays it when active. */}
              <div style={{ height: "100%", display: previewOpen ? "none" : "block" }}>
                {activePanel === "volcano" && (
                  <Volcano
                    rows={allRows}
                    padjCutoff={padjCutoff}
                    log2fcCutoff={log2fcCutoff}
                    pub={pub}
                    displayName={displayName}
                    onHover={setHoveredGene}
                    onToggleLabel={toggleLabel}
                    selectedGene={selectedGene}
                    onSelectGene={setSelectedGene}
                    onRendered={setRenderMs}
                    registerExport={registerExport}
                  />
                )}
                {activePanel === "ma" && (
                  <MAPlot
                    rows={allRows}
                    padjCutoff={padjCutoff}
                    log2fcCutoff={log2fcCutoff}
                    pub={pub}
                    displayName={displayName}
                    onHover={setHoveredGene}
                    onToggleLabel={toggleLabel}
                    selectedGene={selectedGene}
                    onSelectGene={setSelectedGene}
                    onRendered={setRenderMs}
                    registerExport={registerExport}
                  />
                )}
              </div>
              {previewOpen && (
                <div style={{ position: "absolute", inset: 0, display: "flex", flexDirection: "column", background: "#fff" }}>
                  {/* Left-aligned; the top-right is left clear for the mode toggle. */}
                  <div style={{ display: "flex", alignItems: "center", gap: 8, padding: "4px 8px", fontSize: 11, borderBottom: "1px solid rgba(127,127,127,0.2)" }}>
                    <strong>Export preview</strong>
                    <button onClick={() => setSettingsOpen((o) => !o)} style={{ padding: "1px 8px", fontSize: 11, fontWeight: settingsOpen ? 600 : 400 }} title="Publication settings">Settings…</button>
                    <button onClick={() => setFitTick((t) => t + 1)} style={{ padding: "1px 8px", fontSize: 11 }} title="Fit to window">Fit</button>
                    <button onClick={() => setManualOffsets({})} style={{ padding: "1px 8px", fontSize: 11 }} title="Clear manual label positions">Reset labels</button>
                    <div style={{ flex: 1 }} />
                    <button
                      onClick={handleCopySvg}
                      disabled={!canExport}
                      style={{ padding: "1px 8px", fontSize: 11, background: copied ? "#0072B2" : undefined, color: copied ? "#fff" : undefined, borderColor: copied ? "#0072B2" : undefined }}
                      title="Copy as SVG — paste into PowerPoint/Word as a vector graphic"
                    >
                      {copied ? "Copied ✓" : "Copy SVG"}
                    </button>
                    <button onClick={() => handleExport("svg")} disabled={!canExport} style={{ padding: "1px 8px", fontSize: 11 }} title="Save as SVG (choose location)">Save SVG…</button>
                    <button onClick={() => handleExport("png")} disabled={!canExport} style={{ padding: "1px 8px", fontSize: 11 }} title="Save as PNG (choose location)">Save PNG…</button>
                  </div>
                  <ExportPreview
                    series={previewFig.series}
                    xTitle={previewFig.xTitle}
                    yTitle={previewFig.yTitle}
                    shapes={previewFig.shapes}
                    labelPoints={previewLabelPoints}
                    hoverFormat={previewFig.hoverFormat}
                    size={exportSize}
                    manualOffsets={manualOffsets}
                    onManualMove={onManualMove}
                    onToggleLabel={toggleLabel}
                    onHover={setHoveredGene}
                    selectedGene={selectedGene}
                    onSelectGene={setSelectedGene}
                    fitTick={fitTick}
                  />
                  {settingsOpen && (
                    <div style={{ position: "absolute", top: 34, right: 8, zIndex: 30, width: 250, background: "#fff", border: "1px solid rgba(127,127,127,0.4)", borderRadius: 6, boxShadow: "0 6px 20px rgba(0,0,0,0.18)", padding: 10, display: "flex", flexDirection: "column", gap: 8, fontSize: 12 }}>
                      <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
                        <strong style={{ flex: 1 }}>Publication settings</strong>
                        <span style={{ opacity: 0.5, fontSize: 11 }}>{activePanel === "volcano" ? "Volcano" : "MA"}</span>
                        <button onClick={() => setSettingsOpen(false)} style={{ padding: "0 6px", fontSize: 14, lineHeight: 1 }} title="Close">×</button>
                      </div>
                      <label style={SETTING_ROW} title="X and Y axis lengths (plot area)">
                        Axis length (mm)
                        <span style={SETTING_VAL}>
                          <input type="number" min={5} value={widthMm} onChange={(e) => setWidthMm(Number(e.target.value) || 1)} style={{ width: 48 }} />×
                          <input type="number" min={5} value={heightMm} onChange={(e) => setHeightMm(Number(e.target.value) || 1)} style={{ width: 48 }} />
                        </span>
                      </label>
                      <label style={SETTING_ROW}>
                        DPI
                        <span style={SETTING_VAL}><input type="number" min={72} max={1200} value={dpi} onChange={(e) => setDpi(Number(e.target.value) || 72)} style={{ width: 64 }} /></span>
                      </label>
                      <label style={SETTING_ROW}>
                        Font size (pt)
                        <span style={SETTING_VAL}><input type="number" min={4} max={40} value={fontSize} onChange={(e) => setFontSize(Number(e.target.value) || 7)} style={{ width: 64 }} /></span>
                      </label>
                      <label style={SETTING_ROW} title="Gene-label font size">
                        Label font (pt)
                        <span style={SETTING_VAL}><input type="number" min={3} max={40} step={0.5} value={labelFontSize} onChange={(e) => setLabelFontSize(Number(e.target.value) || 5)} style={{ width: 64 }} /></span>
                      </label>
                      <label style={SETTING_ROW}>
                        Point radius (mm)
                        <span style={SETTING_VAL}><input type="number" min={0.05} step={0.05} value={pointRadiusMm} onChange={(e) => setPointRadiusMm(Number(e.target.value) || 0.3)} style={{ width: 64 }} /></span>
                      </label>
                      <label style={SETTING_ROW}>
                        Axis width (pt)
                        <span style={SETTING_VAL}><input type="number" min={0.1} step={0.1} value={axisWidthPt} onChange={(e) => setAxisWidthPt(Number(e.target.value) || 0.5)} style={{ width: 64 }} /></span>
                      </label>
                      <label style={SETTING_ROW}>
                        X range
                        <span style={SETTING_VAL}>
                          <input value={xMin} onChange={(e) => setXMin(e.target.value)} placeholder="min" style={{ width: 40 }} />
                          <input value={xMax} onChange={(e) => setXMax(e.target.value)} placeholder="max" style={{ width: 40 }} />
                          <input value={xTick} onChange={(e) => setXTick(e.target.value)} placeholder="step" style={{ width: 40 }} title="tick interval (blank = auto)" />
                        </span>
                      </label>
                      <label style={SETTING_ROW}>
                        Y range
                        <span style={SETTING_VAL}>
                          <input value={yMin} onChange={(e) => setYMin(e.target.value)} placeholder="min" style={{ width: 40 }} />
                          <input value={yMax} onChange={(e) => setYMax(e.target.value)} placeholder="max" style={{ width: 40 }} />
                          <input value={yTick} onChange={(e) => setYTick(e.target.value)} placeholder="step" style={{ width: 40 }} title="tick interval (blank = auto)" />
                        </span>
                      </label>
                      <span style={{ opacity: 0.55, fontSize: 11 }}>
                        {labeledGenes.size} gene label(s) — check rows in the table to add. Settings are per panel.
                      </span>
                    </div>
                  )}
                </div>
              )}
              </div>
            </div>
            <div
              style={{
                flex: "1 1 40%",
                minWidth: 320,
                border: "1px solid rgba(127,127,127,0.25)",
                borderRadius: 6,
                overflow: "hidden",
                display: "flex",
                flexDirection: "column",
              }}
            >
              <ResultsTable
                rows={filteredRows}
                sortKey={sortKey}
                sortDir={sortDir}
                onSort={handleSort}
                onHover={setHoveredGene}
                labeledGenes={labeledGenes}
                onToggleLabel={toggleLabel}
                geneNameMode={geneNameMode}
                displayName={displayName}
                selectedGene={selectedGene}
              />
            </div>
          </div>

        </>
      )}
    </div>
  );
}

function FiltersBar({
  padjCutoff,
  log2fcCutoff,
  search,
  degOnly,
  symbolMode,
  sourceLabel,
  symbolNote,
  labelCount,
  onPadjChange,
  onLog2fcChange,
  onSearchChange,
  onDegOnlyChange,
  onSymbolModeChange,
  onClearLabels,
}: {
  padjCutoff: number;
  log2fcCutoff: number;
  search: string;
  degOnly: boolean;
  symbolMode: boolean;
  sourceLabel: string | null;
  symbolNote?: string | null;
  labelCount: number;
  onPadjChange: (v: number) => void;
  onLog2fcChange: (v: number) => void;
  onSearchChange: (v: string) => void;
  onDegOnlyChange: (v: boolean) => void;
  onSymbolModeChange: (v: boolean) => void;
  onClearLabels: () => void;
}) {
  return (
    <div style={{ display: "flex", gap: 16, flexWrap: "wrap", alignItems: "center", fontSize: 12 }}>
      <label style={{ display: "flex", gap: 6, alignItems: "center" }}>
        padj &lt;
        <input
          type="number"
          value={padjCutoff}
          step="0.01"
          min="0"
          max="1"
          onChange={(e) => onPadjChange(Number(e.target.value))}
          style={{ width: 70 }}
        />
      </label>
      <label style={{ display: "flex", gap: 6, alignItems: "center" }}>
        |log2FC| &ge;
        <input
          type="number"
          value={log2fcCutoff}
          step="0.1"
          min="0"
          onChange={(e) => onLog2fcChange(Number(e.target.value))}
          style={{ width: 70 }}
        />
      </label>
      <label
        style={{ display: "flex", gap: 5, alignItems: "center", whiteSpace: "nowrap" }}
        title="Show only DEGs in the table — genes passing the padj and |log2FC| cutoffs above. The plot still shows every point."
      >
        <input type="checkbox" checked={degOnly} onChange={(e) => onDegOnlyChange(e.target.checked)} />
        DEGs only
      </label>
      <label
        style={{ display: "flex", gap: 6, alignItems: "center" }}
        title="Filters the table (not the plot) by gene id or symbol. Check a row to label it on the plot."
      >
        find gene:
        <input
          type="search"
          value={search}
          onChange={(e) => onSearchChange(e.target.value)}
          placeholder="id or symbol"
          style={{ width: 200 }}
        />
      </label>
      <label
        style={{ display: "flex", gap: 5, alignItems: "center", opacity: 0.8, whiteSpace: "nowrap" }}
        title={symbolNote ?? "Show gene symbols (human ENSG / Drosophila FBgn) on the plot and table (id shown when no symbol)"}
      >
        <input type="checkbox" checked={symbolMode} onChange={(e) => onSymbolModeChange(e.target.checked)} />
        Gene symbols{sourceLabel ? ` (${sourceLabel})` : ""}{symbolNote ? " ⚠" : ""}
      </label>
      {labelCount > 0 && (
        <button
          onClick={onClearLabels}
          title="Remove all gene labels from the plot"
          style={{ fontSize: 11, padding: "2px 8px", whiteSpace: "nowrap" }}
        >
          Clear labels ({labelCount})
        </button>
      )}
    </div>
  );
}

function SummaryBar({
  counts,
  shown,
  total,
  hoveredGene,
  renderMs,
}: {
  counts: { up: number; down: number; ns: number; filtered: number } | null;
  shown: number;
  total: number;
  hoveredGene: string | null;
  renderMs: number | null;
}) {
  if (!counts) return null;
  return (
    <div style={{ display: "flex", gap: 12, fontSize: 11, opacity: 0.85, flexWrap: "wrap" }}>
      <span>
        shown <code>{shown.toLocaleString()}</code> / total <code>{total.toLocaleString()}</code>
      </span>
      <span style={{ color: "rgb(213,94,0)" }}>up: {counts.up.toLocaleString()}</span>
      <span style={{ color: "rgb(0,114,178)" }}>down: {counts.down.toLocaleString()}</span>
      <span>ns: {counts.ns.toLocaleString()}</span>
      <span>na: {counts.filtered.toLocaleString()}</span>
      <span>render: <code>{renderMs == null ? "…" : `${renderMs.toFixed(0)} ms`}</code></span>
      <span>hover: <code>{hoveredGene ?? "—"}</code></span>
    </div>
  );
}

function formatError(e: unknown): string {
  if (typeof e === "string") return e;
  if (e instanceof Error) return e.message;
  return JSON.stringify(e);
}
