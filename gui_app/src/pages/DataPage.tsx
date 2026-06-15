// Step 1 of the workflow: get counts + metadata onto the project. See
// gui_ux_improvements.md § 3.2. Two modes: pick an existing pair, or build
// one from featureCounts files. Either way the result is written to
// project.inputs — the single source of truth.

import { forwardRef, useEffect, useRef, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import type { UnlistenFn } from "@tauri-apps/api/event";
import { useAppState } from "../lib/project_context";
import { isTauriContext, validateInputs } from "../lib/tauri";
import { dirOf, validationRequestFromProject } from "../lib/requests";
import { FeatureCountsImport } from "../components/FeatureCountsImport";
import { ValidationPanel } from "../components/ValidationPanel";
import type { ValidationResult } from "../lib/validation";

const TABLE_FILTER = [{ name: "Table", extensions: ["csv", "tsv", "txt"] }];

type Mode = "existing" | "featurecounts";
type DropTarget = "counts" | "metadata";

export function DataPage() {
  const { project, projectPath, updateProject, navigateTo } = useAppState();
  const [mode, setMode] = useState<Mode>("existing");
  const [check, setCheck] = useState<ValidationResult | null>(null);
  const [checking, setChecking] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [dropHover, setDropHover] = useState<DropTarget | null>(null);

  const countsZoneRef = useRef<HTMLDivElement>(null);
  const metaZoneRef = useRef<HTMLDivElement>(null);

  const projectDir = dirOf(projectPath);
  const hasInputs = !!project.inputs.counts && !!project.inputs.metadata;

  function setCounts(path: string) {
    updateProject((p) => ({ ...p, inputs: { ...p.inputs, counts: path } }));
    setCheck(null);
  }
  function setMetadata(path: string) {
    updateProject((p) => ({ ...p, inputs: { ...p.inputs, metadata: path } }));
    setCheck(null);
  }

  // Native (Tauri) file drag-and-drop. Only active in the existing-files mode.
  // The OS drop event is window-level, so we hit-test the drop position
  // against the two zones to decide which input it fills.
  useEffect(() => {
    if (mode !== "existing" || !isTauriContext()) return;
    let unlisten: UnlistenFn | null = null;
    let mounted = true;

    const hitTarget = (x: number, y: number): DropTarget | null => {
      if (within(countsZoneRef.current, x, y)) return "counts";
      if (within(metaZoneRef.current, x, y)) return "metadata";
      return null;
    };

    getCurrentWebview()
      .onDragDropEvent((event) => {
        const p = event.payload as {
          type: string;
          position?: { x: number; y: number };
          paths?: string[];
        };
        const dpr = window.devicePixelRatio || 1;
        const x = (p.position?.x ?? 0) / dpr;
        const y = (p.position?.y ?? 0) / dpr;
        if (p.type === "over" || p.type === "enter") {
          setDropHover(hitTarget(x, y));
        } else if (p.type === "leave") {
          setDropHover(null);
        } else if (p.type === "drop") {
          const target = hitTarget(x, y);
          setDropHover(null);
          const first = p.paths?.[0];
          if (!first || !target) return;
          if (target === "counts") setCounts(first);
          else setMetadata(first);
        }
      })
      .then((u) => {
        if (!mounted) {
          u();
          return;
        }
        unlisten = u;
      });

    return () => {
      mounted = false;
      if (unlisten) unlisten();
      setDropHover(null);
    };
  }, [mode]);

  async function pick(target: DropTarget) {
    setError(null);
    const picked = await open({ multiple: false, directory: false, filters: TABLE_FILTER });
    if (!picked) return;
    const path = Array.isArray(picked) ? picked[0] : picked;
    if (target === "counts") setCounts(path);
    else setMetadata(path);
  }

  async function runCheck() {
    setChecking(true);
    setError(null);
    try {
      // Data step checks the data only; the contrast/design is set up later in
      // Analyze, so skip design checks here (test/control aren't chosen yet).
      const r = await validateInputs(validationRequestFromProject(project, projectDir, false));
      setCheck(r);
    } catch (e) {
      setError(formatError(e));
    } finally {
      setChecking(false);
    }
  }

  return (
    <div style={{ padding: 16, display: "flex", flexDirection: "column", gap: 14, height: "100%", overflow: "auto" }}>
      <StepHeader step={1} title="Data" subtitle="Choose the counts and metadata for this analysis." />

      <div style={{ display: "flex", gap: 16, fontSize: 13 }}>
        <label style={{ display: "flex", gap: 6, alignItems: "center", cursor: "pointer" }}>
          <input type="radio" checked={mode === "existing"} onChange={() => setMode("existing")} />
          Use existing files
        </label>
        <label style={{ display: "flex", gap: 6, alignItems: "center", cursor: "pointer" }}>
          <input type="radio" checked={mode === "featurecounts"} onChange={() => setMode("featurecounts")} />
          Build from featureCounts
        </label>
      </div>

      {mode === "existing" ? (
        <section style={{ display: "flex", flexDirection: "column", gap: 8 }}>
          <DropRow
            ref={countsZoneRef}
            label="Counts"
            value={project.inputs.counts}
            placeholder="drop or browse a counts table (gene_id + one column per sample)"
            hovered={dropHover === "counts"}
            onPick={() => pick("counts")}
          />
          <DropRow
            ref={metaZoneRef}
            label="Metadata"
            value={project.inputs.metadata}
            placeholder="drop or browse a metadata table (sample_id + condition, batch, …)"
            hovered={dropHover === "metadata"}
            onPick={() => pick("metadata")}
          />
          <p style={{ fontSize: 11, opacity: 0.55, margin: 0 }}>
            Drag files onto a row, or use Browse. Metadata sample_id values must match
            the counts sample-column names.
          </p>
        </section>
      ) : (
        <section style={{ border: "1px solid rgba(127,127,127,0.25)", borderRadius: 6, padding: 12 }}>
          <FeatureCountsImport
            onProduced={(counts, metadata) => {
              updateProject((p) => ({ ...p, inputs: { counts, metadata } }));
              setCheck(null);
            }}
          />
        </section>
      )}

      <section style={{ display: "flex", gap: 8, alignItems: "center" }}>
        <button onClick={runCheck} disabled={!hasInputs || checking}>
          {checking ? "Checking…" : "Check inputs"}
        </button>
        <PrimaryButton
          onClick={() => navigateTo("analyze")}
          disabled={!hasInputs}
          title={hasInputs ? "Continue to Analyze" : "Set counts and metadata first"}
        >
          Continue to Analyze →
        </PrimaryButton>
      </section>

      {error && <div style={{ color: "#b00020", fontSize: 12, whiteSpace: "pre-wrap" }}>{error}</div>}

      {check && (
        <section style={{ border: "1px solid rgba(127,127,127,0.25)", borderRadius: 6, padding: 10 }}>
          <ValidationPanel result={check} />
        </section>
      )}
    </div>
  );
}

const DropRow = forwardRef<
  HTMLDivElement,
  {
    label: string;
    value: string;
    placeholder: string;
    hovered: boolean;
    onPick: () => void;
  }
>(function DropRow({ label, value, placeholder, hovered, onPick }, ref) {
  return (
    <div style={{ display: "grid", gridTemplateColumns: "100px 1fr auto", gap: 8, alignItems: "center", fontSize: 13 }}>
      <label>{label}</label>
      <div
        ref={ref}
        style={{
          fontSize: 12,
          padding: "8px 10px",
          background: hovered ? "rgba(0,114,178,0.12)" : "rgba(127,127,127,0.08)",
          border: `1px ${hovered ? "dashed #0072B2" : "solid rgba(127,127,127,0.2)"}`,
          borderRadius: 4,
          wordBreak: "break-all",
          minHeight: 18,
        }}
      >
        {value ? (
          <code style={{ opacity: 0.9 }}>{value}</code>
        ) : (
          <span style={{ opacity: 0.45 }}>{placeholder}</span>
        )}
      </div>
      <button onClick={onPick}>Browse…</button>
    </div>
  );
});

export function StepHeader({ step, title, subtitle }: { step: number; title: string; subtitle: string }) {
  return (
    <header style={{ display: "flex", alignItems: "baseline", gap: 10 }}>
      <span
        style={{
          fontSize: 12,
          fontWeight: 700,
          color: "#0072B2",
          border: "1px solid #0072B2",
          borderRadius: 12,
          padding: "1px 9px",
        }}
      >
        {step}
      </span>
      <h2 style={{ margin: 0, fontSize: 18 }}>{title}</h2>
      <span style={{ fontSize: 12, opacity: 0.65 }}>{subtitle}</span>
    </header>
  );
}

export function PrimaryButton({
  children,
  onClick,
  disabled,
  title,
}: {
  children: React.ReactNode;
  onClick: () => void;
  disabled?: boolean;
  title?: string;
}) {
  return (
    <button
      onClick={onClick}
      disabled={disabled}
      title={title}
      style={{
        background: disabled ? undefined : "#0072B2",
        color: disabled ? undefined : "#fff",
        borderColor: disabled ? undefined : "#0072B2",
        fontWeight: 600,
      }}
    >
      {children}
    </button>
  );
}

function within(el: HTMLElement | null, x: number, y: number): boolean {
  if (!el) return false;
  const r = el.getBoundingClientRect();
  return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

function formatError(e: unknown): string {
  if (typeof e === "string") return e;
  if (e instanceof Error) return e.message;
  return JSON.stringify(e);
}
