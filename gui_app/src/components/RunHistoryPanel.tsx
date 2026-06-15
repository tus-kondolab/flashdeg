// Project run history. Displays project.runs[] as a sortable list with
// actions to re-open a result in the Results tab or remove an entry.

import { useMemo } from "react";
import type { Contrast, Design, RunParams, RunRecord } from "../lib/project";

interface RunHistoryPanelProps {
  runs: RunRecord[];
  onOpenInResults: (path: string) => void;
  onRemove?: (index: number) => void;
  /** Apply a run's saved settings back to the current project. */
  onRestore?: (params: RunParams) => void;
  emptyHint?: string;
  /** Limit rows shown; useful for "latest N" inline summaries. */
  limit?: number;
}

export function RunHistoryPanel({
  runs,
  onOpenInResults,
  onRemove,
  onRestore,
  emptyHint = "No runs recorded yet.",
  limit,
}: RunHistoryPanelProps) {
  // Newest first (most recently appended). Each row tracks its original
  // index so onRemove can target the right entry in project.runs.
  const ordered = useMemo(
    () =>
      runs
        .map((r, i) => ({ run: r, index: i }))
        .sort((a, b) => {
          const ta = new Date(a.run.timestamp).getTime();
          const tb = new Date(b.run.timestamp).getTime();
          return tb - ta;
        }),
    [runs],
  );

  if (ordered.length === 0) {
    return (
      <div style={{ padding: 12, opacity: 0.6, fontSize: 12 }}>{emptyHint}</div>
    );
  }

  const display = typeof limit === "number" ? ordered.slice(0, limit) : ordered;

  return (
    <div style={{ overflow: "auto" }}>
      {display.map(({ run, index }) => (
        <div
          key={`${index}-${run.timestamp}`}
          style={{ padding: "8px 0", borderBottom: "1px solid rgba(127,127,127,0.12)" }}
        >
          {/* Line 1: timestamp + actions (never wraps). */}
          <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <span title={run.timestamp} style={{ fontSize: 12, fontWeight: 500, whiteSpace: "nowrap" }}>
              {formatTimestamp(run.timestamp)}
            </span>
            <span style={{ marginLeft: "auto", display: "flex", gap: 4, flexShrink: 0 }}>
              <button onClick={() => onOpenInResults(run.results_path)} style={{ padding: "1px 8px", fontSize: 11 }}>
                Open
              </button>
              {onRestore && run.params && (
                <button
                  onClick={() => onRestore(run.params!)}
                  style={{ padding: "1px 8px", fontSize: 11 }}
                  title="Apply this run's settings to the current project (overwrites the current design/contrast/options/inputs)"
                >
                  Restore
                </button>
              )}
              {onRemove && (
                <button
                  onClick={() => onRemove(index)}
                  style={{ padding: "1px 8px", fontSize: 11 }}
                  title="Remove this entry from project.runs (does not delete the result file)"
                >
                  Remove
                </button>
              )}
            </span>
          </div>
          {/* Line 2: results path, one line, ellipsized from the LEFT (rtl) so
              the tail (…/runs/<ts>/results.csv) stays visible. Full on hover. */}
          <div
            title={run.results_path}
            style={{
              fontFamily: "var(--mono)", fontSize: 11, opacity: 0.75, marginTop: 2,
              whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis",
              direction: "rtl", textAlign: "left",
            }}
          >
            {run.results_path}
          </div>
          {/* Line 3: the settings this run used (provenance). */}
          {run.params ? (
            <details style={{ marginTop: 3 }}>
              <summary style={{ cursor: "pointer", fontSize: 11, opacity: 0.6 }}>settings</summary>
              <div style={{ marginTop: 3, display: "grid", gridTemplateColumns: "auto 1fr", gap: "1px 8px", fontSize: 11 }}>
                {paramRows(run.params).map(([k, v]) => (
                  <div key={k} style={{ display: "contents" }}>
                    <span style={{ opacity: 0.55, whiteSpace: "nowrap" }}>{k}</span>
                    <span style={{ fontFamily: "var(--mono)", wordBreak: "break-word" }}>{v}</span>
                  </div>
                ))}
              </div>
            </details>
          ) : (
            <div style={{ fontSize: 11, opacity: 0.4, marginTop: 3 }}>(no saved settings)</div>
          )}
        </div>
      ))}
      {typeof limit === "number" && ordered.length > limit && (
        <div style={{ fontSize: 11, opacity: 0.6, padding: "4px 0" }}>
          {ordered.length - limit} older run(s) hidden.
        </div>
      )}
    </div>
  );
}

function formatTimestamp(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return iso;
  const pad = (n: number) => n.toString().padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

function baseName(path: string): string {
  const sep = path.includes("\\") ? "\\" : "/";
  const i = path.lastIndexOf(sep);
  return i >= 0 ? path.slice(i + 1) : path;
}

function designSummary(d: Design): string {
  switch (d.kind) {
    case "formula":
      return d.formula;
    case "matrix":
      return `matrix: ${baseName(d.path)}`;
    case "custom":
      return "full custom (see Extra args)";
  }
}

function contrastSummary(c: Contrast): string {
  switch (c.kind) {
    case "factor_levels":
      return `${c.factor}: ${c.test} vs ${c.control}`;
    case "design_column":
      return `column: ${c.name}`;
    case "vector":
      return `vector [${c.values.join(", ")}]`;
  }
}

/** Compact key/value summary of a run's parameters for the history details. */
function paramRows(p: RunParams): [string, string][] {
  const rows: [string, string][] = [
    ["Counts", baseName(p.inputs.counts)],
    ["Metadata", baseName(p.inputs.metadata)],
    ["Design", designSummary(p.design)],
    ["Contrast", contrastSummary(p.contrast)],
    ["Fit / size", `${p.options.fit_type} / ${p.options.size_factors}`],
    [
      "Filters",
      [
        p.options.independent_filter ? "indep" : null,
        p.options.cooks_filter ? "cooks" : null,
        p.options.refit_cooks ? "refit" : null,
      ].filter(Boolean).join(", ") || "none",
    ],
  ];
  if (p.options.extra_args && p.options.extra_args.trim()) {
    rows.push(["Extra args", p.options.extra_args]);
  }
  rows.push(["Threads", String(p.threads)]);
  return rows;
}
