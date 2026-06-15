// Project file schema. Mirrors src-tauri/src/project.rs.
//
// The Rust side is the source of truth for the JSON wire format; this file
// must be kept in sync. Round-trip tests in both layers should catch drift.

export type Contrast =
  | { kind: "factor_levels"; factor: string; test: string; control: string }
  | { kind: "design_column"; name: string }
  | { kind: "vector"; values: number[] };

export type Design =
  | { kind: "formula"; formula: string }
  | { kind: "matrix"; path: string; columns: string[] }
  /** Fully custom: the user supplies the whole model via Options.extra_args.
   *  build_args emits no managed --design/--contrast/--ref-level/--test flags. */
  | { kind: "custom" };

export interface Inputs {
  counts: string;
  metadata: string;
}

export interface Options {
  fit_type: "parametric" | "local" | "mean";
  size_factors: "ratio" | "poscounts";
  independent_filter: boolean;
  cooks_filter: boolean;
  refit_cooks: boolean;
  /** Statistical test. "wald" = standard two-group DEG; "lrt" = likelihood-ratio
   *  test of the full design against a nested reduced model (reduced model is
   *  derived at run time from the design, not stored). */
  test_kind: "wald" | "lrt";
  /** Raw extra CLI flags appended to the flashdeg command (split with
   *  shell-words at run time, no shell). */
  extra_args: string;
}

/** Snapshot of the analysis config a run was launched with (for provenance +
 *  "Restore" in the run history). */
export interface RunParams {
  inputs: Inputs;
  design: Design;
  contrast: Contrast;
  ref_levels: Record<string, string>;
  options: Options;
  threads: number;
}

export interface RunRecord {
  timestamp: string;
  results_path: string;
  flashdeg_version: string;
  git_revision: string;
  /** Parameters used for this run (absent for runs recorded before this field). */
  params?: RunParams;
}

export interface Project {
  schema_version: number;
  inputs: Inputs;
  design: Design;
  contrast: Contrast;
  ref_levels: Record<string, string>;
  options: Options;
  runs: RunRecord[];
}

export const CURRENT_SCHEMA_VERSION = 1;

export function newDefaultProject(): Project {
  return {
    schema_version: CURRENT_SCHEMA_VERSION,
    inputs: { counts: "", metadata: "" },
    design: { kind: "formula", formula: "~ condition" },
    contrast: {
      kind: "factor_levels",
      factor: "condition",
      test: "treated",
      control: "control",
    },
    ref_levels: { condition: "control" },
    options: {
      fit_type: "parametric",
      size_factors: "ratio",
      independent_filter: true,
      cooks_filter: true,
      refit_cooks: true,
      test_kind: "wald",
      extra_args: "",
    },
    runs: [],
  };
}
