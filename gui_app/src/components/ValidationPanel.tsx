// Findings display. See gui_plan.md § 6 / § 7.

import { groupFindings, type Finding, type ValidationResult } from "../lib/validation";

const SEVERITY_STYLES = {
  block: {
    color: "rgb(178, 34, 34)",
    bg: "rgba(178, 34, 34, 0.08)",
    border: "rgba(178, 34, 34, 0.35)",
    label: "BLOCK",
  },
  warn: {
    color: "rgb(180, 95, 6)",
    bg: "rgba(213, 94, 0, 0.08)",
    border: "rgba(213, 94, 0, 0.35)",
    label: "WARN",
  },
} as const;

export function ValidationPanel({ result }: { result: ValidationResult | null }) {
  if (!result) {
    return (
      <div style={{ padding: 12, opacity: 0.6, fontSize: 12 }}>
        Run validation to inspect your counts, metadata, and design.
      </div>
    );
  }

  const { blockers, warnings } = groupFindings(result.findings);
  const clean = blockers.length === 0 && warnings.length === 0;

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
      <Summary
        blockers={blockers.length}
        warnings={warnings.length}
        clean={clean}
        result={result}
      />
      {blockers.length > 0 && <FindingsList title="Blockers" findings={blockers} />}
      {warnings.length > 0 && <FindingsList title="Warnings" findings={warnings} />}
      <InspectionSummary result={result} />
    </div>
  );
}

function Summary({
  blockers,
  warnings,
  clean,
  result,
}: {
  blockers: number;
  warnings: number;
  clean: boolean;
  result: ValidationResult;
}) {
  const hasInputs = result.counts_summary || result.metadata_summary;
  if (clean && hasInputs) {
    return (
      <div
        style={{
          padding: "8px 12px",
          borderRadius: 6,
          background: "rgba(34, 139, 34, 0.1)",
          border: "1px solid rgba(34, 139, 34, 0.35)",
          color: "rgb(20, 100, 20)",
          fontSize: 13,
        }}
      >
        No issues found.
      </div>
    );
  }
  return (
    <div style={{ display: "flex", gap: 12, fontSize: 12 }}>
      <span>
        blockers:{" "}
        <strong style={{ color: SEVERITY_STYLES.block.color }}>{blockers}</strong>
      </span>
      <span>
        warnings:{" "}
        <strong style={{ color: SEVERITY_STYLES.warn.color }}>{warnings}</strong>
      </span>
    </div>
  );
}

function FindingsList({ title, findings }: { title: string; findings: Finding[] }) {
  return (
    <div>
      <h4 style={{ margin: "8px 0 6px", fontSize: 12, textTransform: "uppercase", opacity: 0.7 }}>
        {title} ({findings.length})
      </h4>
      <ul style={{ listStyle: "none", padding: 0, margin: 0, display: "flex", flexDirection: "column", gap: 6 }}>
        {findings.map((f, i) => (
          <FindingRow key={`${f.code}-${i}`} finding={f} />
        ))}
      </ul>
    </div>
  );
}

function FindingRow({ finding }: { finding: Finding }) {
  const style = SEVERITY_STYLES[finding.severity];
  return (
    <li
      style={{
        padding: "8px 10px",
        borderRadius: 6,
        background: style.bg,
        border: `1px solid ${style.border}`,
        fontSize: 12,
        display: "flex",
        flexDirection: "column",
        gap: 4,
      }}
    >
      <div style={{ display: "flex", alignItems: "center", gap: 8, color: style.color }}>
        <strong>{style.label}</strong>
        <code style={{ fontSize: 11, opacity: 0.8 }}>{finding.code}</code>
        <span style={{ fontSize: 11, opacity: 0.6 }}>{finding.category}</span>
      </div>
      <div>{finding.message}</div>
      {finding.context && (
        <div style={{ fontSize: 11, opacity: 0.7 }}>
          <strong>where:</strong> {finding.context}
        </div>
      )}
      {finding.suggested_fix && (
        <div style={{ fontSize: 11, opacity: 0.85 }}>
          <strong>fix:</strong> {finding.suggested_fix}
        </div>
      )}
    </li>
  );
}

function InspectionSummary({ result }: { result: ValidationResult }) {
  if (!result.counts_summary && !result.metadata_summary) return null;
  return (
    <details style={{ fontSize: 11, opacity: 0.85 }}>
      <summary style={{ cursor: "pointer" }}>Inspection details</summary>
      <div style={{ padding: "6px 0", display: "flex", flexDirection: "column", gap: 4 }}>
        {result.counts_summary && (
          <div>
            <strong>counts:</strong>{" "}
            {result.counts_summary.gene_count.toLocaleString()} genes ×{" "}
            {result.counts_summary.sample_count} samples,
            gene_id column: <code>{result.counts_summary.gene_id_column}</code>,
            delimiter:{" "}
            <code>{result.counts_summary.delimiter === "\t" ? "TAB" : result.counts_summary.delimiter}</code>
          </div>
        )}
        {result.metadata_summary && (
          <div>
            <strong>metadata:</strong>{" "}
            {result.metadata_summary.sample_count} samples,
            sample_id column: <code>{result.metadata_summary.sample_id_column}</code>,
            other columns: {result.metadata_summary.columns
              .filter((c) => c !== result.metadata_summary!.sample_id_column)
              .map((c) => <code key={c} style={{ marginRight: 4 }}>{c}</code>)}
          </div>
        )}
      </div>
    </details>
  );
}
