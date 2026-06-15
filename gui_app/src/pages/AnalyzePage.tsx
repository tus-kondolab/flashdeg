// Step 2 of the workflow: set up the comparison, validate, and run. See
// gui_ux_improvements.md § 3.2. Edits the project's design/contrast/options
// directly (single source of truth). FlashDEG binary + threads are app
// settings. Validation is inline; blockers disable Run.

import { Fragment, useCallback, useEffect, useMemo, useRef, useState } from "react";
import { save } from "@tauri-apps/plugin-dialog";
import { useAppState } from "../lib/project_context";
import {
  cancelAnalysis,
  inspectMetadata,
  isTauriContext,
  listenToRun,
  runAnalysis,
  validateExtraArgs,
  validateInputs,
} from "../lib/tauri";
import type { MetadataSummary } from "../lib/validation";
import {
  analysisRequestFromProject,
  buildFormula,
  dirOf,
  isCategoricalColumn,
  isSingleFactorFormula,
  joinPath,
  makeRunOutputPath,
  parseFormula,
  reconcileFactorLevels,
  validationRequestFromProject,
} from "../lib/requests";
import type { Contrast, Design, Project } from "../lib/project";
import type { DoneEvent, LogEvent, RunStatus, StartedEvent } from "../lib/analysis";
import type { RunParams, RunRecord } from "../lib/project";
import { groupFindings, type ValidationResult } from "../lib/validation";
import { ValidationPanel } from "../components/ValidationPanel";
import { ProfileSummary } from "../components/ProfileSummary";
import { RunHistoryPanel } from "../components/RunHistoryPanel";
import { StepHeader, PrimaryButton } from "./DataPage";

const MAX_LOG_LINES = 2000;

interface LogLine {
  stream: "stdout" | "stderr";
  text: string;
}

/** The "What to test" choice in the Advanced mode. */
type TestMode = "two_group" | "any_difference" | "interaction" | "custom";

/** The design modes, each keeping its parameters independently. */
type LeafMode = "basic" | TestMode;

/** The parameter set a mode owns. Each mode keeps its own snapshot so switching
 *  modes never mutates another mode's setup. `extra_args` is per-mode too, so a
 *  Custom mode's raw model args never leak into the other modes' commands. */
interface ModeParams {
  design: Design;
  contrast: Contrast;
  ref_levels: Record<string, string>;
  test_kind: "wald" | "lrt";
  extra_args: string;
}

function snapshotParams(p: Project): ModeParams {
  return {
    design: p.design,
    contrast: p.contrast,
    ref_levels: p.ref_levels,
    test_kind: p.options.test_kind ?? "wald",
    extra_args: p.options.extra_args ?? "",
  };
}

function applyParams(p: Project, mp: ModeParams): Project {
  return {
    ...p,
    design: mp.design,
    contrast: mp.contrast,
    ref_levels: mp.ref_levels,
    options: { ...p.options, test_kind: mp.test_kind, extra_args: mp.extra_args },
  };
}

/** Initial Basic/Multi-factor view for a project. Basic is reserved for a
 *  single-factor *Wald* two-group comparison; an LRT (even on one factor, e.g.
 *  `~ condition` vs `~ 1`) opens in Multi-factor, where the LRT radio lives. */
function initialDesignMode(p: Project): "basic" | "multifactor" {
  const single = p.design.kind === "formula" && isSingleFactorFormula(p.design.formula);
  const isLrt = (p.options.test_kind ?? "wald") === "lrt";
  return single && !isLrt ? "basic" : "multifactor";
}

export function AnalyzePage() {
  const {
    project,
    projectPath,
    projectEpoch,
    updateProject,
    binaryPath,
    threads,
    setThreads,
    setPendingResultsPath,
    navigateTo,
  } = useAppState();

  const [status, setStatus] = useState<RunStatus>({ kind: "idle" });
  const [logs, setLogs] = useState<LogLine[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [autoscroll, setAutoscroll] = useState(true);
  const [validation, setValidation] = useState<ValidationResult | null>(null);
  const [validating, setValidating] = useState(false);
  const [forceRun, setForceRun] = useState(false);
  const [outputOverride, setOutputOverride] = useState<string | null>(null);
  const [meta, setMeta] = useState<MetadataSummary | null>(null);
  const [metaError, setMetaError] = useState<string | null>(null);
  const [contrastError, setContrastError] = useState<string | null>(null);
  const [designMode, setDesignMode] = useState<"basic" | "multifactor">(() =>
    initialDesignMode(project),
  );
  // Custom-options panel open/closed. Auto-opens when the Custom design mode
  // becomes active (that's where the model is typed)…
  const [optionsOpen, setOptionsOpen] = useState<boolean>(project.design.kind === "custom");
  useEffect(() => {
    if (project.design.kind === "custom") setOptionsOpen(true);
  }, [project.design.kind]);
  // …and reset open/closed whenever the whole project is replaced (New / Open).
  useEffect(() => {
    setOptionsOpen(project.design.kind === "custom");
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [projectEpoch]);

  // Reset the Basic/Advanced view whenever the project is replaced (New / Open)
  // — keyed on projectEpoch so it fires even when the path is unchanged (e.g.
  // New from an untitled project).
  useEffect(() => {
    setDesignMode(initialDesignMode(project));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [projectEpoch]);

  const logBoxRef = useRef<HTMLDivElement>(null);
  const startedAtRef = useRef(0);
  const lastOutputRef = useRef("");
  // Snapshot of the config this run was launched with, recorded into the run
  // history on success (captured at run start so later edits don't change it).
  const lastParamsRef = useRef<RunParams | null>(null);

  const projectDir = dirOf(projectPath);
  // Folder of the input counts file — used as the starting location for the
  // "where to save results" dialog when the project is unsaved.
  const countsDir = dirOf(project.inputs.counts);

  // Reset validation whenever the inputs/design/contrast change.
  useEffect(() => {
    setValidation(null);
  }, [project.inputs.counts, project.inputs.metadata, project.contrast, project.design]);

  // When the input counts file changes, drop any previously chosen output
  // location so the next run defaults to the NEW counts file's folder instead
  // of reusing the folder picked for the previous file.
  useEffect(() => {
    setOutputOverride(null);
  }, [project.inputs.counts]);

  // Inspect metadata so factor / level pickers can be dropdowns (item 5).
  useEffect(() => {
    if (!project.inputs.metadata || !isTauriContext()) {
      setMeta(null);
      setMetaError(null);
      return;
    }
    let cancelled = false;
    inspectMetadata(project.inputs.metadata)
      .then((s) => {
        if (cancelled) return;
        setMeta(s);
        setMetaError(null);
        // Drop stale template defaults (e.g. treated/control) that don't
        // exist in this metadata, and auto-pick the factor column.
        updateProject((p) => reconcileFactorLevels(p, s));
      })
      .catch((e) => {
        if (!cancelled) {
          setMeta(null);
          setMetaError(formatError(e));
        }
      });
    return () => {
      cancelled = true;
    };
  }, [project.inputs.metadata, updateProject]);

  // Subscribe to backend run events.
  useEffect(() => {
    if (!isTauriContext()) return;
    let unlisten: (() => void) | null = null;
    let mounted = true;
    listenToRun({
      onStarted: (e: StartedEvent) => {
        if (!mounted) return;
        setLogs([{ stream: "stdout", text: `$ ${e.binary_path} ${quoteArgs(e.args)}` }]);
      },
      onLog: (e: LogEvent) => {
        if (!mounted) return;
        setLogs((prev) => appendLog(prev, e));
      },
      onDone: (e: DoneEvent) => {
        if (!mounted) return;
        const elapsed = Date.now() - startedAtRef.current;
        if (e.cancelled) {
          setStatus({ kind: "cancelled", run_id: e.run_id, duration_ms: elapsed });
          return;
        }
        setStatus({ kind: "done", run_id: e.run_id, exit_code: e.exit_code, duration_ms: elapsed, profile: e.profile });
        if (e.exit_code === 0 && lastOutputRef.current) {
          const record: RunRecord = {
            timestamp: new Date().toISOString(),
            results_path: lastOutputRef.current,
            flashdeg_version: readMeta(e.profile?.metadata, "flashdeg_version") ?? "unknown",
            git_revision: readMeta(e.profile?.metadata, "git_revision") ?? "unknown",
            params: lastParamsRef.current ?? undefined,
          };
          updateProject((prev) => ({ ...prev, runs: [...prev.runs, record] }));
          setPendingResultsPath(lastOutputRef.current);
          navigateTo("results");
        }
      },
    })
      .then((u) => {
        if (!mounted) { u(); return; }
        unlisten = u;
      })
      .catch(() => {
        // not in a Tauri context / listener setup failed
      });
    return () => {
      mounted = false;
      if (unlisten) unlisten();
    };
  }, [setPendingResultsPath, navigateTo, updateProject]);

  useEffect(() => {
    if (!autoscroll || !logBoxRef.current) return;
    logBoxRef.current.scrollTop = logBoxRef.current.scrollHeight;
  }, [logs, autoscroll]);

  const isRunning = status.kind === "running";
  const blockerCount = useMemo(
    () => (validation ? groupFindings(validation.findings).blockers.length : 0),
    [validation],
  );

  const inputsReady = !!project.inputs.counts && !!project.inputs.metadata;

  const handleValidate = useCallback(async () => {
    if (!inputsReady) {
      setError("Set counts and metadata in the Data step first.");
      return;
    }
    setValidating(true);
    setError(null);
    try {
      const r = await validateInputs(validationRequestFromProject(project, projectDir));
      setValidation(r);
    } catch (e) {
      setError(formatError(e));
    } finally {
      setValidating(false);
    }
  }, [project, projectDir, inputsReady]);

  const handleRun = useCallback(async () => {
    if (!binaryPath) {
      setError("Set the FlashDEG binary in Preferences (File ▸ Preferences…, Ctrl/Cmd+,).");
      return;
    }
    if (!inputsReady) {
      setError("Set counts and metadata in the Data step first.");
      return;
    }
    if (contrastError) {
      setError(contrastError);
      return;
    }
    // Resolve the output path once, here, at run time — never on render — so
    // the path we show, the path we run, and the recorded path all agree, and
    // each run gets a unique timestamped folder.
    let output = outputOverride ?? makeRunOutputPath(projectPath);
    if (!output) {
      // Unsaved project and no override: ask where to write, starting in the
      // input counts folder.
      const defaultPath = countsDir ? joinPath(countsDir, "results.csv") : "results.csv";
      const picked = await save({ defaultPath, filters: [{ name: "CSV", extensions: ["csv"] }] });
      if (!picked) return;
      output = picked;
      setOutputOverride(picked);
    }
    if (blockerCount > 0 && !forceRun) {
      setError(`Validation reports ${blockerCount} blocker(s). Resolve them or check "Force run anyway".`);
      return;
    }
    setError(null);
    setLogs([]);
    startedAtRef.current = Date.now();
    lastOutputRef.current = output;
    lastParamsRef.current = {
      inputs: project.inputs,
      design: project.design,
      contrast: project.contrast,
      ref_levels: project.ref_levels,
      options: project.options,
      threads,
    };
    try {
      const req = analysisRequestFromProject(project, {
        binaryPath,
        outputPath: output,
        threads,
        projectDir,
      });
      const runId = await runAnalysis(req);
      setStatus({ kind: "running", run_id: runId, started_at: startedAtRef.current });
    } catch (e) {
      const msg = formatError(e);
      setStatus({ kind: "failed", run_id: "-", message: msg });
      setError(msg);
    }
  }, [binaryPath, inputsReady, contrastError, outputOverride, projectPath, countsDir, blockerCount, forceRun, project, threads, projectDir]);

  const handleCancel = useCallback(async () => {
    if (status.kind !== "running") return;
    try {
      await cancelAnalysis(status.run_id);
    } catch (e) {
      setError(formatError(e));
    }
  }, [status]);

  return (
    <div style={{ padding: 16, display: "flex", flexDirection: "column", gap: 14, height: "100%", overflow: "auto" }}>
      <StepHeader step={2} title="Analyze" subtitle="Set up the comparison, check it, and run." />

      {!inputsReady && (
        <div style={{ fontSize: 12, color: "rgb(140,75,5)", background: "rgba(213,94,0,0.1)", padding: "8px 12px", borderRadius: 6 }}>
          No counts/metadata set yet. Go to the <strong>Data</strong> step first.
        </div>
      )}

      <MetadataPreview meta={meta} metaError={metaError} hasMetadata={!!project.inputs.metadata} />

      <DesignContrastEditor
        project={project}
        updateProject={updateProject}
        meta={meta}
        onContrastError={setContrastError}
        mode={designMode}
        setMode={setDesignMode}
        projectEpoch={projectEpoch}
      />

      <OptionsEditor project={project} updateProject={updateProject} threads={threads} setThreads={setThreads} open={optionsOpen} onOpenChange={setOptionsOpen} />

      <section style={{ display: "flex", gap: 10, alignItems: "center", fontSize: 12 }}>
        <span style={{ opacity: 0.7 }}>results →</span>
        <code style={{ fontSize: 11, opacity: 0.85, wordBreak: "break-all", flex: 1 }}>
          {outputOverride
            ? outputOverride
            : projectDir
              ? `${projectDir}${projectDir.includes("\\") ? "\\" : "/"}runs/<timestamp>/results.csv  (new folder each run)`
              : countsDir
                ? `(project unsaved — Run will ask where to save, starting in ${countsDir})`
                : "(project unsaved — you'll be asked where to save on Run)"}
        </code>
        {outputOverride && (
          <button
            onClick={() => setOutputOverride(null)}
            style={{ padding: "2px 8px", fontSize: 11 }}
            title="Switch back to the automatic per-run folder"
          >
            auto
          </button>
        )}
        <button
          onClick={async () => {
            const picked = await save({ defaultPath: "results.csv", filters: [{ name: "CSV", extensions: ["csv"] }] });
            if (picked) setOutputOverride(picked);
          }}
          style={{ padding: "2px 8px", fontSize: 11 }}
        >
          change…
        </button>
      </section>

      <section style={{ display: "flex", gap: 8, alignItems: "center", flexWrap: "wrap" }}>
        <button onClick={handleValidate} disabled={validating || !inputsReady}>
          {validating ? "Checking…" : "Check"}
        </button>
        <PrimaryButton
          onClick={handleRun}
          disabled={isRunning || !inputsReady || !!contrastError || (blockerCount > 0 && !forceRun)}
        >
          {isRunning ? "Running…" : "Run"}
        </PrimaryButton>
        <button onClick={handleCancel} disabled={!isRunning}>Cancel</button>
        <StatusPill status={status} />
        {blockerCount > 0 && (
          <label style={{ fontSize: 12, display: "flex", alignItems: "center", gap: 4 }}>
            <input type="checkbox" checked={forceRun} onChange={(e) => setForceRun(e.target.checked)} />
            Force run anyway ({blockerCount})
          </label>
        )}
        <label style={{ fontSize: 12, display: "flex", alignItems: "center", gap: 4, marginLeft: 12 }}>
          <input type="checkbox" checked={autoscroll} onChange={(e) => setAutoscroll(e.target.checked)} />
          autoscroll
        </label>
        <div style={{ flex: 1 }} />
        <span style={{ fontSize: 11, opacity: 0.6 }}>{logs.length} log lines</span>
      </section>

      {error && <div style={{ color: "#b00020", fontSize: 12, whiteSpace: "pre-wrap" }}>{error}</div>}

      {validation && (
        <section style={{ border: "1px solid rgba(127,127,127,0.25)", borderRadius: 6, padding: 10, maxHeight: 220, overflow: "auto", flexShrink: 0 }}>
          <ValidationPanel result={validation} />
        </section>
      )}

      <div
        ref={logBoxRef}
        style={{
          minHeight: 120,
          maxHeight: 260,
          flexShrink: 0,
          padding: 8,
          background: "rgba(127,127,127,0.06)",
          border: "1px solid rgba(127,127,127,0.25)",
          borderRadius: 6,
          fontFamily: "var(--mono)",
          fontSize: 11,
          lineHeight: 1.45,
          overflow: "auto",
          whiteSpace: "pre-wrap",
          wordBreak: "break-word",
        }}
      >
        {logs.length === 0 && <div style={{ opacity: 0.5 }}>(no output yet) — click Run.</div>}
        {logs.map((l, i) => (
          <div key={i} style={{ color: l.stream === "stderr" ? "rgb(180, 95, 6)" : undefined }}>{l.text}</div>
        ))}
      </div>

      {status.kind === "done" && status.profile && <ProfileSummary profile={status.profile} />}

      {project.runs.length > 0 && (
        <section style={{ border: "1px solid rgba(127,127,127,0.25)", borderRadius: 6, padding: 8 }}>
          <header style={{ display: "flex", alignItems: "baseline", gap: 8, marginBottom: 4 }}>
            <strong style={{ fontSize: 12 }}>Recent runs</strong>
            <span style={{ fontSize: 11, opacity: 0.6 }}>latest {Math.min(project.runs.length, 3)} of {project.runs.length}</span>
          </header>
          <RunHistoryPanel
            runs={project.runs}
            onOpenInResults={(path) => { setPendingResultsPath(path); navigateTo("results"); }}
            limit={3}
          />
        </section>
      )}
    </div>
  );
}

function MetadataPreview({
  meta,
  metaError,
  hasMetadata,
}: {
  meta: MetadataSummary | null;
  metaError: string | null;
  hasMetadata: boolean;
}) {
  if (!hasMetadata) return null;
  if (metaError) {
    return (
      <div style={{ fontSize: 11, color: "rgb(140,30,30)" }}>
        metadata: could not read ({metaError})
      </div>
    );
  }
  if (!meta) return null;
  const otherCols = meta.columns.filter((c) => c !== meta.sample_id_column);
  return (
    <div
      style={{
        display: "flex",
        alignItems: "center",
        gap: 12,
        flexWrap: "wrap",
        padding: "8px 12px",
        background: "rgba(0,114,178,0.08)",
        border: "1px solid rgba(0,114,178,0.35)",
        borderRadius: 6,
        fontSize: 13,
      }}
    >
      <span>
        <strong style={{ fontSize: 16, color: "#0072B2" }}>{meta.sample_count}</strong> samples
      </span>
      <span style={{ opacity: 0.5 }}>│</span>
      <span>
        sample id: <code style={{ fontWeight: 600 }}>{meta.sample_id_column}</code>
      </span>
      <span style={{ opacity: 0.5 }}>│</span>
      <span style={{ display: "flex", alignItems: "center", gap: 6, flexWrap: "wrap" }}>
        columns:
        {otherCols.length === 0 ? (
          <span style={{ opacity: 0.6 }}>(none besides sample id)</span>
        ) : (
          otherCols.map((c) => (
            <code
              key={c}
              style={{
                padding: "1px 8px",
                background: "rgba(0,114,178,0.15)",
                borderRadius: 10,
                fontSize: 12,
              }}
            >
              {c}
            </code>
          ))
        )}
      </span>
    </div>
  );
}

/** A select when options are known, otherwise a free-text input. Fixed width
 *  so adjacent pickers line up regardless of their option text length. */
function Picker({
  value,
  options,
  onChange,
  placeholder,
  width = 200,
}: {
  value: string;
  options: string[];
  onChange: (v: string) => void;
  placeholder?: string;
  width?: number;
}) {
  const style: React.CSSProperties = { width, boxSizing: "border-box" };
  if (options.length === 0) {
    return (
      <input
        value={value}
        onChange={(e) => onChange(e.target.value)}
        placeholder={placeholder}
        style={style}
      />
    );
  }
  const opts = options.includes(value) || value === "" ? options : [value, ...options];
  return (
    <select value={value} onChange={(e) => onChange(e.target.value)} style={style}>
      {value === "" && <option value="">(choose…)</option>}
      {opts.map((o) => (
        <option key={o} value={o}>{o}</option>
      ))}
    </select>
  );
}

function DesignContrastEditor({
  project,
  updateProject,
  meta,
  onContrastError,
  mode,
  setMode,
  projectEpoch,
}: {
  project: Project;
  updateProject: (m: (p: Project) => Project) => void;
  meta: MetadataSummary | null;
  onContrastError: (msg: string | null) => void;
  mode: "basic" | "multifactor";
  setMode: (m: "basic" | "multifactor") => void;
  projectEpoch: number;
}) {
  const contrast = project.contrast;
  const [freeText, setFreeText] = useState(false);
  const testKind = project.options.test_kind ?? "wald";

  // Per-mode parameter cache (session). Each mode (basic / two-group / any
  // difference / interaction) keeps its own {design, contrast, ref_levels,
  // test_kind}; switching modes snapshots the one you leave and restores the one
  // you enter, so no mode's setup ever bleeds into another. Reset when the
  // project is replaced (Open changes the path).
  const modeCache = useRef<Partial<Record<LeafMode, ModeParams>>>({});
  const lastMfLeafRef = useRef<LeafMode>("two_group");
  // Drop per-mode drafts and the edit-as-text toggle whenever the whole project
  // is replaced (New / Open), so no prior project's modes linger.
  useEffect(() => {
    modeCache.current = {};
    setFreeText(false);
  }, [projectEpoch]);

  // Clear any stale contrast error when the test kind changes (custom-vector
  // contrasts — the only source of such errors — were removed from the builder).
  useEffect(() => {
    onContrastError(null);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [contrast.kind]);

  function setContrast(c: Contrast) {
    updateProject((p) => ({ ...p, contrast: c }));
  }
  function setRefLevel(factor: string, level: string) {
    updateProject((p) => ({
      ...p,
      ref_levels: level ? { ...p.ref_levels, [factor]: level } : omit(p.ref_levels, factor),
    }));
  }
  function setDesignFormula(formula: string) {
    updateProject((p) => ({ ...p, design: { kind: "formula", formula } as Design }));
  }

  // The primary (tested) factor lives in the contrast; covariates live in the
  // design formula. Keep them coherent on every edit.
  const formula = project.design.kind === "formula" ? project.design.formula : "";
  const parsed = parseFormula(formula);
  const primary = contrast.kind === "factor_levels" ? contrast.factor : parsed.primary;
  const covariates = parsed.covariates.filter((c) => c !== primary);

  function setPrimaryFactor(f: string) {
    updateProject((p) => {
      const prevFactor = p.contrast.kind === "factor_levels" ? p.contrast.factor : "";
      const changed = f !== prevFactor;
      // Changing the factor invalidates the old test/control levels, so reset
      // them (and the now-stale reference levels) — the user re-picks.
      const c =
        p.contrast.kind === "factor_levels"
          ? changed
            ? { kind: "factor_levels" as const, factor: f, test: "", control: "" }
            : { ...p.contrast, factor: f }
          : p.contrast;
      let design = p.design;
      if (p.design.kind === "formula" && !parseFormula(p.design.formula).hasInteraction) {
        const covs = parseFormula(p.design.formula).covariates.filter((x) => x !== f);
        design = { kind: "formula", formula: buildFormula(f, covs) };
      }
      let ref_levels = p.ref_levels;
      if (changed) {
        ref_levels = { ...p.ref_levels };
        delete ref_levels[prevFactor];
        delete ref_levels[f];
      }
      return { ...p, contrast: c, design, ref_levels };
    });
  }
  function toggleCovariate(col: string) {
    updateProject((p) => {
      if (p.design.kind !== "formula") return p;
      const t = parseFormula(p.design.formula);
      const prim = p.contrast.kind === "factor_levels" ? p.contrast.factor : t.primary;
      const set = new Set(t.covariates.filter((c) => c !== prim));
      if (set.has(col)) set.delete(col);
      else set.add(col);
      return { ...p, design: { kind: "formula", formula: buildFormula(prim, [...set]) } };
    });
  }

  // Basic ↔ Multi-factor toggle. Routed through switchLeaf so each mode keeps
  // its own parameters; Multi-factor returns to whichever sub-mode you last used.
  function selectMode(m: "basic" | "multifactor") {
    switchLeaf(m === "basic" ? "basic" : lastMfLeafRef.current);
  }

  const factorColumns = meta ? meta.columns.filter((c) => c !== meta.sample_id_column) : [];
  const levelValues =
    meta && primary ? Object.keys(meta.column_value_distributions[primary] ?? {}) : [];
  const otherColumns = factorColumns.filter((c) => c !== primary);

  // The primary factor's reference = its control (auto). Covariates still
  // need a manual baseline, so only categorical covariates get ref pickers.
  const refFactors = covariates.filter((c) => c && (!meta || isCategoricalColumn(meta, c)));

  // --- Interaction term: pick two factors -> auto-build design + term ---
  const levelsOf = (f: string) =>
    meta ? Object.keys(meta.column_value_distributions[f] ?? {}) : [];
  // Lexicographically sorted levels — matches FlashDEG (std::set in design.cpp),
  // so the default reference (sorted-first) is exactly the one FlashDEG would
  // pick with no --ref-level.
  const sortedLevelsOf = (f: string) => [...levelsOf(f)].sort();
  // The reference (baseline) for a factor: the user's choice if still valid,
  // else FlashDEG's default (sorted-first level).
  const pickRef = (f: string, refs: Record<string, string>) => {
    const lv = sortedLevelsOf(f);
    return refs[f] && lv.includes(refs[f]) ? refs[f] : lv[0] ?? "";
  };
  // The default tested (non-reference) level used in the interaction column.
  const defaultTest = (f: string, ref: string) => sortedLevelsOf(f).find((v) => v !== ref) ?? "";

  // Build `~ A * B` and the DESeq2-style interaction column `A[T.la]:B[T.lb]`,
  // AND write each factor's reference into ref_levels so build_args emits
  // `--ref-level`. This is the fix for "contrast column not found": the GUI no
  // longer guesses FlashDEG's default reference — it sets it explicitly, so the
  // generated column name always exists in the design matrix.
  function withInteraction(
    p: Project, a: string, b: string, refA: string, refB: string, testA: string, testB: string,
  ): Project {
    const name = a && b && testA && testB ? `${a}[T.${testA}]:${b}[T.${testB}]` : "";
    const formula = a && b ? `~ ${a} * ${b}` : `~ ${a || b}`;
    const ref_levels = { ...p.ref_levels };
    if (a && refA) ref_levels[a] = refA;
    if (b && refB) ref_levels[b] = refB;
    return { ...p, design: { kind: "formula", formula } as Design, contrast: { kind: "design_column", name }, ref_levels };
  }
  // Change one (or both) interaction factors; references persist per-factor via
  // ref_levels, tested levels reset to default when their factor changes.
  function setInteraction(a: string, b: string) {
    updateProject((p) => {
      const prev = p.contrast.kind === "design_column" ? parseInteractionName(p.contrast.name) : null;
      const refA = pickRef(a, p.ref_levels);
      const refB = pickRef(b, p.ref_levels);
      const testA = prev && prev.a === a && prev.la && prev.la !== refA ? prev.la : defaultTest(a, refA);
      const testB = prev && prev.b === b && prev.lb && prev.lb !== refB ? prev.lb : defaultTest(b, refB);
      return withInteraction(p, a, b, refA, refB, testA, testB);
    });
  }
  const setInteractionRef = (slot: "a" | "b", level: string) => {
    updateProject((p) => {
      const cur = p.contrast.kind === "design_column" ? parseInteractionName(p.contrast.name) : null;
      if (!cur) return p;
      const refA = slot === "a" ? level : pickRef(cur.a, p.ref_levels);
      const refB = slot === "b" ? level : pickRef(cur.b, p.ref_levels);
      const testA = cur.la && cur.la !== refA ? cur.la : defaultTest(cur.a, refA);
      const testB = cur.lb && cur.lb !== refB ? cur.lb : defaultTest(cur.b, refB);
      return withInteraction(p, cur.a, cur.b, refA, refB, testA, testB);
    });
  };
  const setInteractionTest = (slot: "a" | "b", level: string) => {
    updateProject((p) => {
      const cur = p.contrast.kind === "design_column" ? parseInteractionName(p.contrast.name) : null;
      if (!cur) return p;
      const refA = pickRef(cur.a, p.ref_levels);
      const refB = pickRef(cur.b, p.ref_levels);
      return withInteraction(p, cur.a, cur.b, refA, refB, slot === "a" ? level : cur.la, slot === "b" ? level : cur.lb);
    });
  };
  const iaParsed = contrast.kind === "design_column" ? parseInteractionName(contrast.name) : null;
  const iaA = iaParsed?.a ?? "";
  const iaB = iaParsed?.b ?? "";
  const iaLa = iaParsed?.la ?? "";
  const iaLb = iaParsed?.lb ?? "";
  const iaRefA = iaA ? pickRef(iaA, project.ref_levels) : "";
  const iaRefB = iaB ? pickRef(iaB, project.ref_levels) : "";

  // The "What to test" choice in Multi-factor mode. "Two-group comparison" and
  // "Any difference across groups" both use a factor_levels contrast and differ
  // only by test kind (Wald vs LRT); "Interaction effect" uses a design_column
  // contrast and is always Wald.
  const testMode: TestMode =
    project.design.kind === "custom"
      ? "custom"
      : contrast.kind === "design_column"
        ? "interaction"
        : testKind === "lrt"
          ? "any_difference"
          : "two_group";

  // The currently-active mode, and a record of the last Multi-factor sub-mode so
  // the Basic→Multi-factor toggle returns there.
  const currentLeaf: LeafMode = mode === "basic" ? "basic" : testMode;
  if (currentLeaf !== "basic") lastMfLeafRef.current = currentLeaf;

  // Default parameters for a mode that has no cached snapshot yet. Seeds the
  // factor from the current one (so the same factor can be tested different
  // ways), but is otherwise self-contained.
  function defaultParamsForLeaf(target: LeafMode): ModeParams {
    const f = primary || iaA || factorColumns[0] || "";
    if (target === "custom") {
      // Fully custom: no managed model; the user writes the model in extra_args.
      return {
        design: { kind: "custom" },
        contrast: { kind: "factor_levels", factor: f, test: "", control: "" },
        ref_levels: {},
        test_kind: "wald",
        extra_args: "",
      };
    }
    if (target === "interaction") {
      const a = f;
      const b = factorColumns.find((c) => c !== a) ?? "";
      const refA = pickRef(a, {});
      const refB = pickRef(b, {});
      const testA = defaultTest(a, refA);
      const testB = defaultTest(b, refB);
      const name = a && b && testA && testB ? `${a}[T.${testA}]:${b}[T.${testB}]` : "";
      const formula = a && b ? `~ ${a} * ${b}` : `~ ${a || b}`;
      const ref_levels: Record<string, string> = {};
      if (a && refA) ref_levels[a] = refA;
      if (b && refB) ref_levels[b] = refB;
      return { design: { kind: "formula", formula }, contrast: { kind: "design_column", name }, ref_levels, test_kind: "wald", extra_args: "" };
    }
    return {
      design: { kind: "formula", formula: buildFormula(f, []) },
      contrast: { kind: "factor_levels", factor: f, test: "", control: "" },
      ref_levels: {},
      test_kind: target === "any_difference" ? "lrt" : "wald",
      extra_args: "",
    };
  }

  // Switch design mode. Snapshot the mode we leave and restore the one we enter
  // (or seed defaults) so each mode keeps its own independent parameters.
  function switchLeaf(target: LeafMode) {
    if (target === currentLeaf) {
      setMode(target === "basic" ? "basic" : "multifactor");
      return;
    }
    onContrastError(null);
    setFreeText(false);
    modeCache.current = { ...modeCache.current, [currentLeaf]: snapshotParams(project) };
    const next = modeCache.current[target] ?? defaultParamsForLeaf(target);
    updateProject((p) => applyParams(p, next));
    setMode(target === "basic" ? "basic" : "multifactor");
  }

  function selectTestMode(m: TestMode) {
    switchLeaf(m);
  }

  const test = contrast.kind === "factor_levels" ? contrast.test : "";
  const control = contrast.kind === "factor_levels" ? contrast.control : "";

  // Auto-fill the display contrast for LRT mode. In "Any difference across
  // groups" the test/baseline only label which log2FoldChange is shown (the
  // LRT itself covers every level), so default them to "first non-reference vs
  // reference" instead of forcing the user to choose. Fires on entering the
  // mode, changing the factor, or when metadata first loads. Never overwrites a
  // contrast the user already filled.
  useEffect(() => {
    if (testMode !== "any_difference" || contrast.kind !== "factor_levels") return;
    if (test && control) return;
    if (!meta || !primary) return;
    const vals = Object.keys(meta.column_value_distributions[primary] ?? {});
    if (vals.length < 2) return;
    const baseline =
      project.ref_levels[primary] && vals.includes(project.ref_levels[primary])
        ? project.ref_levels[primary]
        : vals[0];
    const testLevel = vals.find((v) => v !== baseline) ?? "";
    if (!testLevel || !baseline) return;
    updateProject((p) => {
      if (p.contrast.kind !== "factor_levels" || (p.contrast.test && p.contrast.control)) return p;
      return {
        ...p,
        contrast: { ...p.contrast, test: testLevel, control: baseline },
        ref_levels: { ...p.ref_levels, [p.contrast.factor]: baseline },
      };
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [testMode, primary, meta, contrast.kind, test, control]);
  const setTest = (v: string) =>
    contrast.kind === "factor_levels" && setContrast({ ...contrast, test: v });
  // The control group is the reference level by convention; keep ref_levels
  // in lock-step with the chosen control so the user never sets it manually.
  const setControl = (v: string) => {
    updateProject((p) => {
      if (p.contrast.kind !== "factor_levels") return p;
      const factor = p.contrast.factor;
      const ref_levels = v ? { ...p.ref_levels, [factor]: v } : omit(p.ref_levels, factor);
      return { ...p, contrast: { ...p.contrast, control: v }, ref_levels };
    });
  };

  const compareLabelStyle: React.CSSProperties = { fontSize: 12, opacity: 0.9 };
  const compareRow = (
    <div style={{ display: "grid", gridTemplateColumns: "180px auto", gap: "6px 10px", alignItems: "center", fontSize: 13 }}>
      <span style={compareLabelStyle}>Factor</span>
      <Picker value={primary} options={factorColumns} onChange={setPrimaryFactor} placeholder="condition" />

      <span style={compareLabelStyle}>Test group</span>
      <Picker value={test} options={levelValues} onChange={setTest} placeholder="treated" />

      <span style={compareLabelStyle}>Reference group (baseline)</span>
      <Picker value={control} options={levelValues} onChange={setControl} placeholder="control" />
    </div>
  );

  return (
    <section style={{ border: "1px solid rgba(127,127,127,0.25)", borderRadius: 6, padding: 12, display: "flex", flexDirection: "column", gap: 10 }}>
      <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
        <strong style={{ fontSize: 13 }}>Mode</strong>
        <div style={{ display: "flex" }}>
          {(["basic", "multifactor"] as const).map((m) => {
            const isLeft = m === "basic";
            const self = mode === m ? "#0072B2" : "rgba(127,127,127,0.45)";
            return (
              <button
                key={m}
                onClick={() => selectMode(m)}
                style={{
                  boxSizing: "border-box",
                  borderTop: `1px solid ${self}`,
                  borderBottom: `1px solid ${self}`,
                  borderLeft: isLeft ? `1px solid ${self}` : "none",
                  // The middle edge (left button's right border) is the
                  // separator — always blue.
                  borderRight: isLeft ? "1px solid #0072B2" : `1px solid ${self}`,
                  borderTopLeftRadius: isLeft ? 6 : 0,
                  borderBottomLeftRadius: isLeft ? 6 : 0,
                  borderTopRightRadius: isLeft ? 0 : 6,
                  borderBottomRightRadius: isLeft ? 0 : 6,
                  padding: "3px 12px",
                  fontSize: 12,
                  fontWeight: mode === m ? 600 : 400,
                  background: mode === m ? "rgba(0,114,178,0.12)" : "transparent",
                  color: mode === m ? "#0072B2" : "#888",
                  cursor: "pointer",
                }}
              >
                {m === "basic" ? "Basic" : "Advanced"}
              </button>
            );
          })}
        </div>
      </div>

      {project.design.kind === "matrix" ? (
        <div style={{ fontSize: 12, opacity: 0.8 }}>
          Using a precomputed design matrix: <code>{project.design.path}</code>{" "}
          <button onClick={() => setDesignFormula("~ condition")} style={{ padding: "1px 6px", fontSize: 11 }}>
            switch to formula
          </button>
        </div>
      ) : (
        <>
          {mode === "basic" ? (
            compareRow
          ) : (
            <>
              {/* What to test — the first choice in Multi-factor */}
              <div style={{ display: "grid", gridTemplateColumns: "180px auto", gap: "6px 10px", alignItems: "center", fontSize: 13 }}>
                <span style={compareLabelStyle}>What to test</span>
                <div style={{ display: "flex", gap: 16, fontSize: 12, alignItems: "center", flexWrap: "wrap" }}>
                  {([
                    {
                      m: "two_group",
                      label: "Two-group comparison",
                      title: "Compare two groups, e.g. treated vs control (Wald test).",
                    },
                    {
                      m: "any_difference",
                      label: "Any difference across groups",
                      tag: "LRT",
                      title:
                        "Is there ANY difference across all groups of a variable (every timepoint, every condition) — not just two? Likelihood-ratio test.",
                    },
                    {
                      m: "interaction",
                      label: "Interaction effect",
                      title: "Does one factor's effect depend on another factor?",
                    },
                    {
                      m: "custom",
                      label: "Full custom",
                      title: "Specify the whole model yourself via raw CLI arguments (for designs the builder can't make: precomputed matrices, 3-way interactions, interaction LRT, custom contrast vectors, …).",
                    },
                  ] as { m: TestMode; label: string; tag?: string; title: string }[]).map(({ m, label, tag, title }) => (
                    <label key={m} style={{ display: "flex", gap: 4, alignItems: "center", cursor: "pointer" }} title={title}>
                      <input
                        type="radio"
                        checked={testMode === m}
                        onChange={() => selectTestMode(m)}
                      />
                      {label}
                      {tag && (
                        <span
                          style={{
                            fontSize: 9,
                            fontWeight: 700,
                            letterSpacing: 0.3,
                            padding: "0 4px",
                            borderRadius: 3,
                            background: "rgba(0,114,178,0.15)",
                            color: "#0072B2",
                          }}
                        >
                          {tag}
                        </span>
                      )}
                    </label>
                  ))}
                </div>
              </div>

              {testMode === "custom" ? (
                <div style={{ fontSize: 12, opacity: 0.85, lineHeight: 1.6, padding: "4px 0" }}>
                  <div>
                    Write the whole model yourself in{" "}
                    <strong>Custom options → Extra arguments</strong> below.
                  </div>
                  <div>
                    In this mode the builder adds no <code>--design</code> / <code>--contrast</code> /{" "}
                    <code>--test</code>.
                  </div>
                  <div>Examples:</div>
                  <code style={{ display: "block", marginTop: 6, padding: "4px 8px", background: "rgba(127,127,127,0.08)", borderRadius: 4, wordBreak: "break-all" }}>
                    --design "~ g + c + g:c" --test LRT --reduced "~ g + c" --contrast-name "g[T.x]:c[T.y]"
                  </code>
                  <code style={{ display: "block", marginTop: 4, padding: "4px 8px", background: "rgba(127,127,127,0.08)", borderRadius: 4, wordBreak: "break-all" }}>
                    --design-matrix design.csv --contrast-vector 0,0,1
                  </code>
                </div>
              ) : (
                <>
              {/* Contrast-specific input for the chosen test */}
              {contrast.kind === "factor_levels" && (
                <>
                  {compareRow}
                  {testMode === "any_difference" && <ReducedModelLine covariates={covariates} />}
                </>
              )}
              {contrast.kind === "design_column" && (
                <div style={{ display: "grid", gridTemplateColumns: "180px auto", gap: "6px 10px", alignItems: "center", fontSize: 13 }}>
                  <span style={compareLabelStyle}>Interaction of</span>
                  <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                    <Picker value={iaA} options={factorColumns} onChange={(v) => setInteraction(v, iaB)} width={160} />
                    <span style={{ opacity: 0.7 }}>×</span>
                    <Picker value={iaB} options={factorColumns.filter((c) => c !== iaA)} onChange={(v) => setInteraction(iaA, v)} width={160} />
                  </div>

                  {/* Per-factor baseline (reference) + tested level. The baseline
                      is emitted as --ref-level, so the generated column name
                      always matches the design matrix. */}
                  {([
                    { slot: "a" as const, f: iaA, ref: iaRefA, test: iaLa },
                    { slot: "b" as const, f: iaB, ref: iaRefB, test: iaLb },
                  ]).map(({ slot, f, ref, test }) =>
                    f ? (
                      <Fragment key={slot}>
                        <span style={compareLabelStyle}>{f}</span>
                        <div style={{ display: "flex", alignItems: "center", gap: 8, flexWrap: "wrap" }}>
                          <span style={{ fontSize: 11, opacity: 0.6 }}>baseline</span>
                          <Picker value={ref} options={sortedLevelsOf(f)} onChange={(v) => setInteractionRef(slot, v)} width={150} />
                          <span style={{ opacity: 0.7 }}>vs</span>
                          <Picker value={test} options={sortedLevelsOf(f).filter((v) => v !== ref)} onChange={(v) => setInteractionTest(slot, v)} width={150} />
                        </div>
                      </Fragment>
                    ) : null,
                  )}

                  <span style={compareLabelStyle}>Tested term</span>
                  <code style={{ fontSize: 12, padding: "4px 8px", background: "rgba(127,127,127,0.08)", borderRadius: 4, wordBreak: "break-all" }}>
                    {contrast.name || "(pick two factors)"}
                  </code>
                  <span />
                  <span style={{ fontSize: 11, opacity: 0.6 }}>
                    Tests whether one factor's effect differs across the other. <code>baseline</code>{" "}
                    sets each factor's reference level (emitted as <code>--ref-level</code>);{" "}
                    <code>vs</code> is the level whose interaction is tested.
                  </span>
                </div>
              )}

              {/* Adjust for + covariate reference apply to the additive
                  two-group / any-difference builder only. Interaction edits the
                  design as text below. */}
              {contrast.kind === "factor_levels" && (
                <>
                  <div style={{ display: "grid", gridTemplateColumns: "180px 1fr", gap: 8, alignItems: "start", fontSize: 13 }}>
                    <span style={compareLabelStyle}>Adjust for</span>
                    <div style={{ display: "flex", gap: 12, flexWrap: "wrap" }}>
                      {otherColumns.length === 0 ? (
                        <span style={{ fontSize: 11, opacity: 0.6 }}>no other metadata columns</span>
                      ) : (
                        otherColumns.map((c) => (
                          <label key={c} style={{ display: "flex", gap: 4, alignItems: "center", cursor: "pointer", fontSize: 12 }}>
                            <input
                              type="checkbox"
                              checked={covariates.includes(c)}
                              onChange={() => toggleCovariate(c)}
                              disabled={freeText || parsed.hasInteraction}
                            />
                            {c}
                          </label>
                        ))
                      )}
                    </div>
                  </div>

                  {refFactors.length > 0 && (
                    <div style={{ display: "grid", gridTemplateColumns: "180px 1fr", gap: 6, alignItems: "center", fontSize: 13 }}>
                      <span style={compareLabelStyle}>Covariate reference</span>
                      <div style={{ display: "flex", gap: 12, flexWrap: "wrap", alignItems: "center" }}>
                        {refFactors.map((f) => {
                          const vals = meta ? Object.keys(meta.column_value_distributions[f] ?? {}) : [];
                          return (
                            <label key={f} style={{ display: "flex", gap: 4, alignItems: "center", fontSize: 12 }}>
                              {f} →
                              <Picker
                                value={project.ref_levels[f] ?? ""}
                                options={vals}
                                onChange={(v) => setRefLevel(f, v)}
                                placeholder="(choose)"
                                width={140}
                              />
                            </label>
                          );
                        })}
                      </div>
                    </div>
                  )}
                </>
              )}

              {/* Generated design + free-text escape hatch. Same read-only /
                  "edit as text" toggle in every mode. */}
              <div style={{ display: "grid", gridTemplateColumns: "180px 1fr auto", gap: 8, alignItems: "center", fontSize: 12 }}>
                <span style={compareLabelStyle}>Generated design</span>
                {freeText ? (
                  <input value={formula} onChange={(e) => setDesignFormula(e.target.value)} placeholder="~ batch + condition" />
                ) : (
                  <code style={{ padding: "4px 8px", background: "rgba(127,127,127,0.08)", borderRadius: 4 }}>{formula}</code>
                )}
                <button onClick={() => setFreeText((v) => !v)} style={{ padding: "2px 8px", fontSize: 11 }}>
                  {freeText ? "use builder" : "edit as text"}
                </button>
              </div>
                </>
              )}

            </>
          )}
        </>
      )}
    </section>
  );
}

/** Read-only summary of the LRT reduced model (full design minus the tested
 *  factor). Mirrors deriveReducedFormula: `~ covariates` or `~ 1`. */
function ReducedModelLine({ covariates }: { covariates: string[] }) {
  const reduced = covariates.length > 0 ? `~ ${covariates.join(" + ")}` : "~ 1";
  return (
    <div style={{ display: "grid", gridTemplateColumns: "180px auto", gap: "6px 10px", alignItems: "baseline", fontSize: 13 }}>
      <span style={{ fontSize: 12, opacity: 0.9 }}>Reduced model</span>
      <code style={{ fontSize: 12, padding: "4px 8px", background: "rgba(127,127,127,0.08)", borderRadius: 4, alignSelf: "flex-start" }}>
        {reduced}
      </code>
    </div>
  );
}

function OptionsEditor({
  project,
  updateProject,
  threads,
  setThreads,
  open,
  onOpenChange,
}: {
  project: Project;
  updateProject: (m: (p: Project) => Project) => void;
  threads: number;
  setThreads: (n: number) => void;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}) {
  const o = project.options;
  function setOpt<K extends keyof typeof o>(key: K, value: (typeof o)[K]) {
    updateProject((p) => ({ ...p, options: { ...p.options, [key]: value } }));
  }
  return (
    <details
      open={open}
      onToggle={(e) => onOpenChange((e.currentTarget as HTMLDetailsElement).open)}
      style={{ border: "1px solid rgba(127,127,127,0.25)", borderRadius: 6, padding: 12, fontSize: 12 }}
    >
      <summary style={{ cursor: "pointer", fontSize: 13, fontWeight: 600, opacity: 0.85 }}>
        Custom options
        <span style={{ fontWeight: 400, opacity: 0.6, marginLeft: 8, fontSize: 11 }}>
          (defaults match DESeq2 — usually no need to change)
        </span>
      </summary>
      <div style={{ display: "flex", gap: 18, alignItems: "center", flexWrap: "wrap", marginTop: 12 }}>
      <label style={{ display: "flex", gap: 6, alignItems: "center" }}>
        Fit
        <select value={o.fit_type} onChange={(e) => setOpt("fit_type", e.target.value as typeof o.fit_type)}>
          <option value="parametric">parametric</option>
          <option value="local">local</option>
          <option value="mean">mean</option>
        </select>
      </label>
      <label style={{ display: "flex", gap: 6, alignItems: "center" }}>
        Size factors
        <select value={o.size_factors} onChange={(e) => setOpt("size_factors", e.target.value as typeof o.size_factors)}>
          <option value="ratio">ratio</option>
          <option value="poscounts">poscounts</option>
        </select>
      </label>
      <label style={{ display: "flex", gap: 4, alignItems: "center" }}>
        <input type="checkbox" checked={o.independent_filter} onChange={(e) => setOpt("independent_filter", e.target.checked)} />
        Independent filter
      </label>
      <label style={{ display: "flex", gap: 4, alignItems: "center" }}>
        <input type="checkbox" checked={o.cooks_filter} onChange={(e) => setOpt("cooks_filter", e.target.checked)} />
        Cook's filter
      </label>
      <label style={{ display: "flex", gap: 4, alignItems: "center" }}>
        <input type="checkbox" checked={o.refit_cooks} onChange={(e) => setOpt("refit_cooks", e.target.checked)} />
        Refit Cook's
      </label>
      <label style={{ display: "flex", gap: 6, alignItems: "center" }}>
        Threads
        <input type="number" min={1} max={64} value={threads} onChange={(e) => setThreads(Number(e.target.value) || 1)} style={{ width: 56 }} />
      </label>
      </div>
      <ExtraArgsField
        value={project.options.extra_args}
        onChange={(v) => setOpt("extra_args", v)}
      />
    </details>
  );
}

// Free-text extra CLI flags appended to the flashdeg command. Shows a live,
// quote-aware token preview (validated by the same shell-words splitter the run
// uses) so the user sees exactly what will be appended. No shell is involved.
function ExtraArgsField({ value, onChange }: { value: string; onChange: (v: string) => void }) {
  const [preview, setPreview] = useState<{ tokens?: string[]; error?: string }>({});
  useEffect(() => {
    if (!value.trim()) { setPreview({}); return; }
    if (!isTauriContext()) return;
    let cancelled = false;
    const id = window.setTimeout(() => {
      validateExtraArgs(value)
        .then((tokens) => { if (!cancelled) setPreview({ tokens }); })
        .catch((e) => { if (!cancelled) setPreview({ error: formatError(e) }); });
    }, 200);
    return () => { cancelled = true; window.clearTimeout(id); };
  }, [value]);

  return (
    <div style={{ marginTop: 14, borderTop: "1px solid rgba(127,127,127,0.15)", paddingTop: 12 }}>
      <label style={{ display: "block", marginBottom: 4 }}>
        Extra arguments
        <span style={{ opacity: 0.6, marginLeft: 8, fontSize: 11, fontWeight: 400 }}>
          appended to the flashdeg command (no shell; use quotes to group an argument)
        </span>
      </label>
      <input
        type="text"
        value={value}
        onChange={(e) => onChange(e.target.value)}
        placeholder="--lfc-threshold 1 --alpha 0.05"
        spellCheck={false}
        style={{ width: "100%", boxSizing: "border-box", fontFamily: "var(--mono)", fontSize: 12 }}
      />
      <div style={{ fontSize: 11, marginTop: 4, minHeight: 16 }}>
        {preview.error ? (
          <span style={{ color: "#b00020" }}>✗ {preview.error}</span>
        ) : preview.tokens && preview.tokens.length > 0 ? (
          <span style={{ opacity: 0.85 }}>
            → appends {preview.tokens.length} arg(s):{" "}
            {preview.tokens.map((t, i) => (
              <code key={i} style={{ background: "rgba(127,127,127,0.12)", padding: "0 4px", borderRadius: 3, marginRight: 4 }}>{t}</code>
            ))}
          </span>
        ) : (
          <span style={{ opacity: 0.5 }}>e.g. extra DESeq2 flags your build supports.</span>
        )}
      </div>
    </div>
  );
}

function StatusPill({ status }: { status: RunStatus }) {
  const s = (() => {
    switch (status.kind) {
      case "idle": return { bg: "rgba(127,127,127,0.15)", fg: "#555", label: "idle" };
      case "running": return { bg: "rgba(0,114,178,0.15)", fg: "rgb(0,80,140)", label: `running` };
      case "done": return {
        bg: status.exit_code === 0 ? "rgba(34,139,34,0.15)" : "rgba(178,34,34,0.15)",
        fg: status.exit_code === 0 ? "rgb(20,100,20)" : "rgb(140,30,30)",
        label: status.exit_code === 0 ? `done (${(status.duration_ms / 1000).toFixed(1)}s)` : `exit ${status.exit_code}`,
      };
      case "cancelled": return { bg: "rgba(180,95,6,0.15)", fg: "rgb(140,75,5)", label: "cancelled" };
      case "failed": return { bg: "rgba(178,34,34,0.15)", fg: "rgb(140,30,30)", label: "failed" };
    }
  })();
  return (
    <span style={{ padding: "3px 10px", borderRadius: 12, fontSize: 11, background: s.bg, color: s.fg, fontWeight: 500 }}>
      {s.label}
    </span>
  );
}

function omit(obj: Record<string, string>, key: string): Record<string, string> {
  const { [key]: _drop, ...rest } = obj;
  return rest;
}

/** Recover the two factors and their tested levels from an interaction column
 *  name `A[T.la]:B[T.lb]`. */
function parseInteractionName(
  name: string,
): { a: string; la: string; b: string; lb: string } | null {
  const m = name.match(/^(.+?)\[T\.(.+?)\]:(.+?)\[T\.(.+?)\]$/);
  return m ? { a: m[1], la: m[2], b: m[3], lb: m[4] } : null;
}

function appendLog(prev: LogLine[], e: LogEvent): LogLine[] {
  const next = prev.concat({ stream: e.stream, text: e.line });
  return next.length > MAX_LOG_LINES ? next.slice(next.length - MAX_LOG_LINES) : next;
}

function quoteArgs(args: string[]): string {
  return args.map((a) => (a.includes(" ") || a.includes('"') ? `"${a.replace(/"/g, '\\"')}"` : a)).join(" ");
}

function readMeta(meta: Record<string, unknown> | undefined, key: string): string | null {
  if (!meta) return null;
  const v = meta[key];
  return typeof v === "string" ? v : null;
}

function formatError(e: unknown): string {
  if (typeof e === "string") return e;
  if (e instanceof Error) return e.message;
  return JSON.stringify(e);
}
