// Project -> backend-request mapping. This is the *code-level* single source
// of truth: every screen derives its ValidationRequest / AnalysisRequest from
// the open project through these helpers, so no screen invents its own field
// set. See gui_ux_improvements.md § 3.1.

import type { Project } from "./project";
import type { MetadataSummary, ValidationRequest } from "./validation";
import type { AnalysisRequest } from "./analysis";

/** The condition column for validation/guardrails, derived from the contrast. */
export function deriveConditionColumn(p: Project): string {
  if (p.contrast.kind === "factor_levels") return p.contrast.factor;
  const keys = Object.keys(p.ref_levels);
  return keys[0] ?? "";
}

/** Experiment/control groups, meaningful only for factor-levels contrasts. */
export function deriveExperimentControl(p: Project): {
  experiment: string | null;
  control: string | null;
} {
  if (p.contrast.kind === "factor_levels") {
    return { experiment: p.contrast.test, control: p.contrast.control };
  }
  return { experiment: null, control: null };
}

export function dirOf(path: string | null): string | null {
  if (!path) return null;
  const sep = path.includes("\\") ? "\\" : "/";
  const i = path.lastIndexOf(sep);
  return i >= 0 ? path.slice(0, i) : null;
}

export function joinPath(dir: string, name: string): string {
  const sep = dir.includes("\\") ? "\\" : "/";
  return dir.endsWith(sep) ? `${dir}${name}` : `${dir}${sep}${name}`;
}

function timestamp(): string {
  return new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-");
}

/**
 * Build a fresh, unique output path for a run: `<projectDir>/runs/<ts>/results.csv`.
 * Each run gets its own timestamped folder so results AND profile.json never
 * overwrite a previous run. Call this once at run time (not on render) — it
 * embeds the current timestamp. Returns null when the project is unsaved.
 */
export function makeRunOutputPath(projectPath: string | null): string | null {
  const dir = dirOf(projectPath);
  if (!dir) return null;
  const sep = projectPath && projectPath.includes("\\") ? "\\" : "/";
  return [dir, "runs", timestamp(), "results.csv"].join(sep);
}

/** profile.json sits next to the results file. */
export function profilePathFor(outputPath: string): string | null {
  const dir = dirOf(outputPath);
  if (!dir) return null;
  return joinPath(dir, "profile.json");
}

/** Path for an exported plot: `<results dir>/plots/<name>`. */
export function plotFilePath(resultsPath: string, name: string): string {
  const dir = dirOf(resultsPath) ?? ".";
  return joinPath(joinPath(dir, "plots"), name);
}

const FACTOR_SYNONYMS = ["condition", "group", "treatment", "genotype", "cohort", "tissue"];

// Columns whose names suggest a nuisance / batch variable to adjust for.
const BATCH_LIKE = ["batch", "lane", "date", "run", "replicate", "rep", "sex", "donor", "subject"];

export interface DesignTerms {
  /** Factor of interest (tested). Placed LAST in the formula per DESeq2. */
  primary: string;
  /** Additive adjustment covariates (batch etc.), in display order. */
  covariates: string[];
  /** True when the formula contains an interaction (`:` or `*`) this simple
   *  builder can't represent — caller should fall back to free-text. */
  hasInteraction: boolean;
}

/** Build `~ cov1 + cov2 + primary` (interest variable last). */
export function buildFormula(primary: string, covariates: string[]): string {
  const terms = [...covariates.filter((c) => c && c !== primary)];
  if (primary) terms.push(primary);
  return `~ ${terms.join(" + ")}`;
}

/**
 * Parse a design formula into builder terms. Recognises the additive subset
 * `~ a + b + c` (last term = primary). Interaction terms (`:` / `*`) set
 * hasInteraction so the UI can fall back to free-text editing.
 */
export function parseFormula(formula: string): DesignTerms {
  const rhs = formula.replace(/^\s*~\s*/, "").trim();
  const terms = rhs.split("+").map((t) => t.trim()).filter((t) => t.length > 0);
  const hasInteraction = terms.some((t) => t.includes(":") || t.includes("*"));
  const mainTerms = terms.filter((t) => !t.includes(":") && !t.includes("*"));
  const primary = mainTerms.length > 0 ? mainTerms[mainTerms.length - 1] : "";
  const covariates = mainTerms.slice(0, -1);
  return { primary, covariates, hasInteraction };
}

/**
 * Reduced model for a likelihood-ratio test: drop the tested (primary) factor
 * and keep the adjustment covariates. `~ batch + condition` -> `~ batch`;
 * `~ condition` -> `~ 1` (an intercept-only model, i.e. "no grouping at all").
 * Mirrors how the builder places the primary factor last (see buildFormula).
 */
export function deriveReducedFormula(formula: string): string {
  const { covariates } = parseFormula(formula);
  return covariates.length > 0 ? `~ ${covariates.join(" + ")}` : "~ 1";
}

/** True when the formula is a single main-effect term (Basic mode). */
export function isSingleFactorFormula(formula: string): boolean {
  const t = parseFormula(formula);
  return !t.hasInteraction && t.covariates.length === 0 && t.primary !== "";
}

/** A column is categorical if not every value parses as a finite number. */
export function isCategoricalColumn(
  meta: MetadataSummary,
  column: string,
): boolean {
  const dist = meta.column_value_distributions[column];
  if (!dist) return true;
  const values = Object.keys(dist);
  if (values.length === 0) return true;
  return !values.every((v) => v.trim() !== "" && Number.isFinite(Number(v)));
}

/** Batch-like metadata columns (excluding the chosen primary factor). */
export function detectBatchColumns(
  meta: MetadataSummary,
  primary: string,
): string[] {
  return meta.columns
    .filter((c) => c !== meta.sample_id_column && c !== primary)
    .filter((c) => BATCH_LIKE.includes(c.toLowerCase()));
}

/**
 * Reconcile a project's factor-levels contrast against freshly-inspected
 * metadata. Stale defaults (e.g. the template's "treated"/"control") that do
 * not exist in the data are cleared so the dropdowns don't show values the
 * user never chose; the factor is auto-selected from the metadata columns;
 * ref_levels referencing missing columns/values are pruned. Returns the same
 * project object when nothing needs changing (so it won't mark the project
 * dirty). See gui_ux_improvements item: stale contrast defaults.
 */
export function reconcileFactorLevels(p: Project, meta: MetadataSummary): Project {
  if (p.contrast.kind !== "factor_levels") {
    const pruned = pruneRefLevels(p.ref_levels, meta);
    return sameRefLevels(pruned, p.ref_levels) ? p : { ...p, ref_levels: pruned };
  }

  const columns = meta.columns.filter((c) => c !== meta.sample_id_column);
  if (columns.length === 0) return p;

  const { factor, test, control } = p.contrast;
  const newFactor = columns.includes(factor)
    ? factor
    : columns.find((c) => FACTOR_SYNONYMS.includes(c.toLowerCase())) ?? columns[0];

  const values = Object.keys(meta.column_value_distributions[newFactor] ?? {});
  const newTest = values.includes(test) ? test : "";
  const newControl = values.includes(control) ? control : "";

  let refLevels = pruneRefLevels(p.ref_levels, meta);
  if (refLevels[newFactor] === undefined && newControl) {
    refLevels = { ...refLevels, [newFactor]: newControl };
  }

  // Keep a single-factor (Basic) design formula in sync with the factor.
  let design = p.design;
  if (
    p.design.kind === "formula" &&
    isSingleFactorFormula(p.design.formula) &&
    newFactor &&
    parseFormula(p.design.formula).primary !== newFactor
  ) {
    design = { kind: "formula", formula: buildFormula(newFactor, []) };
  }

  const contrastSame = newFactor === factor && newTest === test && newControl === control;
  const designSame = design === p.design;
  if (contrastSame && designSame && sameRefLevels(refLevels, p.ref_levels)) return p;

  return {
    ...p,
    design,
    contrast: { kind: "factor_levels", factor: newFactor, test: newTest, control: newControl },
    ref_levels: refLevels,
  };
}

function pruneRefLevels(
  refLevels: Record<string, string>,
  meta: MetadataSummary,
): Record<string, string> {
  const out: Record<string, string> = {};
  for (const [col, lvl] of Object.entries(refLevels)) {
    const dist = meta.column_value_distributions[col];
    if (dist && Object.prototype.hasOwnProperty.call(dist, lvl)) out[col] = lvl;
  }
  return out;
}

function sameRefLevels(a: Record<string, string>, b: Record<string, string>): boolean {
  const ak = Object.keys(a);
  const bk = Object.keys(b);
  if (ak.length !== bk.length) return false;
  return ak.every((k) => a[k] === b[k]);
}

export function validationRequestFromProject(
  p: Project,
  projectDir: string | null,
  includeDesignChecks = true,
): ValidationRequest {
  const { experiment, control } = deriveExperimentControl(p);
  return {
    counts_path: p.inputs.counts,
    metadata_path: p.inputs.metadata,
    design_formula: p.design.kind === "formula" ? p.design.formula : null,
    contrast: p.contrast,
    ref_levels: p.ref_levels,
    condition_column: deriveConditionColumn(p),
    experiment_group: experiment,
    control_group: control,
    project_dir: projectDir,
    include_design_checks: includeDesignChecks,
  };
}

export function analysisRequestFromProject(
  p: Project,
  opts: {
    binaryPath: string;
    outputPath: string;
    threads: number;
    projectDir: string | null;
  },
): AnalysisRequest {
  // LRT is only wired for formula designs (the reduced model is derived by
  // dropping the tested factor). Matrix designs would need a reduced matrix,
  // which the GUI does not yet provide, so fall back to Wald there.
  const useLrt = p.options.test_kind === "lrt" && p.design.kind === "formula";
  return {
    binary_path: opts.binaryPath,
    counts_path: p.inputs.counts,
    metadata_path: p.inputs.metadata,
    output_path: opts.outputPath,
    profile_json_path: profilePathFor(opts.outputPath),
    design: p.design,
    contrast: p.contrast,
    ref_levels: p.ref_levels,
    fit_type: p.options.fit_type,
    size_factors: p.options.size_factors,
    test_kind: useLrt ? "lrt" : "wald",
    reduced_formula:
      useLrt && p.design.kind === "formula"
        ? deriveReducedFormula(p.design.formula)
        : null,
    independent_filter: p.options.independent_filter,
    cooks_filter: p.options.cooks_filter,
    refit_cooks: p.options.refit_cooks,
    threads: opts.threads,
    project_dir: opts.projectDir,
    extra_args: p.options.extra_args,
  };
}
