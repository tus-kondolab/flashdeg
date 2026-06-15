// featureCounts -> counts.csv + metadata.csv import flow. See gui_plan.md
// § 5.2. Extracted from the old ImportPage so it can live inside the Data
// step. On completion it calls `onProduced(countsPath, metadataPath)`.

import { useCallback, useMemo, useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import {
  inspectFeatureCounts,
  mergeFeatureCounts,
  writeMetadataCsv,
} from "../lib/tauri";
import type {
  FeatureCountsInspection,
  GeneSetDiffPolicy,
  MergeReport,
} from "../lib/featurecounts";

const FC_FILTER = [
  { name: "featureCounts output", extensions: ["txt", "tsv", "fc", "counts"] },
];

interface LoadedFile {
  path: string;
  inspection: FeatureCountsInspection;
  overrides: Record<string, string>;
}

interface MetadataRow {
  sample_id: string;
  condition: string;
}

export function FeatureCountsImport({
  onProduced,
}: {
  onProduced: (countsPath: string, metadataPath: string) => void;
}) {
  const [files, setFiles] = useState<LoadedFile[]>([]);
  const [policy, setPolicy] = useState<GeneSetDiffPolicy>("intersection");
  const [outputCounts, setOutputCounts] = useState<string>("");
  const [outputMetadata, setOutputMetadata] = useState<string>("");
  const [mergeReport, setMergeReport] = useState<MergeReport | null>(null);
  const [metadata, setMetadata] = useState<MetadataRow[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [metadataWritten, setMetadataWritten] = useState(false);

  async function handleAddFiles() {
    setError(null);
    try {
      const picked = await open({ multiple: true, directory: false, filters: FC_FILTER });
      if (!picked) return;
      const paths = Array.isArray(picked) ? picked : [picked];
      const additions: LoadedFile[] = [];
      for (const p of paths) {
        try {
          const insp = await inspectFeatureCounts(p);
          additions.push({ path: p, inspection: insp, overrides: {} });
        } catch (e) {
          setError((prev) => (prev ? prev + "\n" : "") + `Failed to inspect ${p}: ${formatError(e)}`);
        }
      }
      if (additions.length > 0) {
        setFiles((prev) => [...prev, ...additions]);
        setMergeReport(null);
        setMetadata([]);
        setMetadataWritten(false);
      }
    } catch (e) {
      setError(formatError(e));
    }
  }

  function effectiveSampleName(file: LoadedFile, raw: string, inferred: string): string {
    return file.overrides[raw] ?? inferred;
  }

  const handleMerge = useCallback(async () => {
    if (files.length === 0) {
      setError("Add at least one featureCounts file.");
      return;
    }
    if (!outputCounts) {
      setError("Pick an output path for counts.csv first.");
      return;
    }
    setError(null);
    setBusy(true);
    try {
      const r = await mergeFeatureCounts({
        files: files.map((f) => ({ path: f.path, sample_overrides: f.overrides })),
        output_counts_path: outputCounts,
        on_gene_set_diff: policy,
      });
      setMergeReport(r);
      setMetadata(r.sample_names.map((s) => ({ sample_id: s, condition: "" })));
      setMetadataWritten(false);
    } catch (e) {
      setError(formatError(e));
    } finally {
      setBusy(false);
    }
  }, [files, outputCounts, policy]);

  async function handleWriteMetadata() {
    if (!outputMetadata) {
      setError("Pick an output path for metadata.csv first.");
      return;
    }
    if (metadata.length === 0) {
      setError("Run the merge step first.");
      return;
    }
    setError(null);
    setBusy(true);
    try {
      await writeMetadataCsv({
        output_path: outputMetadata,
        sample_id_column: "sample_id",
        sample_names: metadata.map((r) => r.sample_id),
        columns: ["condition"],
        rows: metadata.map((r) => [r.condition]),
      });
      setMetadataWritten(true);
      if (mergeReport) onProduced(mergeReport.output_path, outputMetadata);
    } catch (e) {
      setError(formatError(e));
    } finally {
      setBusy(false);
    }
  }

  const totalSamples = useMemo(
    () => files.reduce((sum, f) => sum + f.inspection.sample_columns.length, 0),
    [files],
  );

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
      <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
        <button onClick={handleAddFiles} disabled={busy}>Add featureCounts files…</button>
        <span style={{ fontSize: 11, opacity: 0.7 }}>
          {files.length} file(s) · {totalSamples} sample column(s)
        </span>
      </div>

      {files.map((f, i) => (
        <div
          key={`${f.path}-${i}`}
          style={{ border: "1px solid rgba(127,127,127,0.25)", borderRadius: 6, padding: 8 }}
        >
          <div style={{ display: "flex", gap: 8, alignItems: "center", marginBottom: 4 }}>
            <code style={{ fontSize: 11, flex: 1, wordBreak: "break-all" }}>{f.path}</code>
            <span style={{ fontSize: 11, opacity: 0.7 }}>
              {f.inspection.data_row_count.toLocaleString()} genes
            </span>
            <button
              onClick={() => {
                setFiles((prev) => prev.filter((_, j) => j !== i));
                setMergeReport(null);
                setMetadata([]);
              }}
              disabled={busy}
              style={{ padding: "1px 8px", fontSize: 11 }}
            >
              Remove
            </button>
          </div>
          <table style={{ borderCollapse: "collapse", fontSize: 11, width: "100%" }}>
            <thead>
              <tr style={{ opacity: 0.7 }}>
                <th style={cell}>column header</th>
                <th style={cell}>sample name</th>
              </tr>
            </thead>
            <tbody>
              {f.inspection.sample_columns.map((col) => (
                <tr key={col.raw_header}>
                  <td style={cell}><code style={{ fontSize: 10 }}>{col.raw_header}</code></td>
                  <td style={cell}>
                    <input
                      type="text"
                      value={effectiveSampleName(f, col.raw_header, col.inferred_sample_name)}
                      onChange={(e) =>
                        setFiles((prev) =>
                          prev.map((pf, j) =>
                            j === i
                              ? { ...pf, overrides: { ...pf.overrides, [col.raw_header]: e.target.value } }
                              : pf,
                          ),
                        )
                      }
                      style={{ width: "100%" }}
                      disabled={busy}
                    />
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ))}

      <div style={{ display: "grid", gridTemplateColumns: "auto 1fr auto auto", gap: 8, alignItems: "center", fontSize: 12 }}>
        <label>output counts.csv</label>
        <input value={outputCounts} onChange={(e) => setOutputCounts(e.target.value)} placeholder="absolute path" />
        <button
          onClick={async () => {
            const picked = await save({ defaultPath: "counts.csv", filters: [{ name: "CSV", extensions: ["csv"] }] });
            if (picked) setOutputCounts(picked);
          }}
          disabled={busy}
        >
          Save as…
        </button>
        <select value={policy} onChange={(e) => setPolicy(e.target.value as GeneSetDiffPolicy)} disabled={busy}>
          <option value="intersection">intersection</option>
          <option value="union">union</option>
          <option value="error">error if differ</option>
        </select>
      </div>

      <div>
        <button onClick={handleMerge} disabled={busy || files.length === 0}>
          {busy ? "Working…" : "Merge → counts.csv"}
        </button>
      </div>

      {error && <div style={{ color: "#b00020", fontSize: 12, whiteSpace: "pre-wrap" }}>{error}</div>}

      {mergeReport && (
        <div
          style={{
            border: "1px solid rgba(34,139,34,0.4)",
            background: "rgba(34,139,34,0.06)",
            borderRadius: 6,
            padding: 8,
            fontSize: 12,
          }}
        >
          Wrote <code>{mergeReport.output_path}</code> — {mergeReport.gene_count.toLocaleString()} genes ×{" "}
          {mergeReport.sample_count} samples.
          {mergeReport.warnings.length > 0 && (
            <ul style={{ margin: "6px 0 0", paddingLeft: 18, color: "rgb(180,95,6)" }}>
              {mergeReport.warnings.map((w, i) => <li key={i}>{w}</li>)}
            </ul>
          )}
        </div>
      )}

      {metadata.length > 0 && (
        <div style={{ border: "1px solid rgba(127,127,127,0.25)", borderRadius: 6, padding: 8, display: "flex", flexDirection: "column", gap: 8 }}>
          <strong style={{ fontSize: 12 }}>Metadata editor</strong>
          <table style={{ borderCollapse: "collapse", fontSize: 12 }}>
            <thead>
              <tr style={{ background: "rgba(127,127,127,0.08)" }}>
                <th style={cell}>sample_id</th>
                <th style={cell}>condition</th>
              </tr>
            </thead>
            <tbody>
              {metadata.map((r, i) => (
                <tr key={r.sample_id}>
                  <td style={cell}><code>{r.sample_id}</code></td>
                  <td style={cell}>
                    <input
                      value={r.condition}
                      onChange={(e) =>
                        setMetadata((prev) => prev.map((m, j) => (j === i ? { ...m, condition: e.target.value } : m)))
                      }
                      placeholder="e.g. control / treated"
                      style={{ width: "100%" }}
                    />
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          <div style={{ display: "grid", gridTemplateColumns: "auto 1fr auto", gap: 8, alignItems: "center", fontSize: 12 }}>
            <label>output metadata.csv</label>
            <input value={outputMetadata} onChange={(e) => setOutputMetadata(e.target.value)} placeholder="absolute path" />
            <button
              onClick={async () => {
                const picked = await save({ defaultPath: "metadata.csv", filters: [{ name: "CSV", extensions: ["csv"] }] });
                if (picked) setOutputMetadata(picked);
              }}
              disabled={busy}
            >
              Save as…
            </button>
          </div>
          <div>
            <button onClick={handleWriteMetadata} disabled={busy || !outputMetadata}>
              Write metadata.csv &amp; use as inputs
            </button>
            {metadataWritten && (
              <span style={{ marginLeft: 8, fontSize: 12, color: "rgb(20,100,20)" }}>
                ✓ inputs set on project
              </span>
            )}
          </div>
        </div>
      )}
    </div>
  );
}

const cell: React.CSSProperties = {
  padding: "4px 8px",
  border: "1px solid rgba(127,127,127,0.15)",
  textAlign: "left",
};

function formatError(e: unknown): string {
  if (typeof e === "string") return e;
  if (e instanceof Error) return e.message;
  return JSON.stringify(e);
}
