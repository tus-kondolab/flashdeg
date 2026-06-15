// Analysis run types. Mirrors src-tauri/src/flashdeg.rs.

import type { Contrast, Design, Options } from "./project";

export interface AnalysisRequest {
  binary_path: string;
  counts_path: string;
  metadata_path: string;
  output_path: string;
  profile_json_path: string | null;
  design: Design;
  contrast: Contrast;
  ref_levels: Record<string, string>;
  fit_type: Options["fit_type"];
  size_factors: Options["size_factors"];
  /** "lrt" emits `--test LRT --reduced <reduced_formula>`; "wald" emits neither. */
  test_kind: Options["test_kind"];
  /** Reduced-model formula for an LRT run; null for Wald. */
  reduced_formula: string | null;
  independent_filter: boolean;
  cooks_filter: boolean;
  refit_cooks: boolean;
  threads: number;
  project_dir: string | null;
  /** Raw extra CLI flags, appended after the managed args (split with
   *  shell-words in the backend, no shell). */
  extra_args: string;
}

export interface StartedEvent {
  run_id: string;
  binary_path: string;
  args: string[];
}

export interface LogEvent {
  run_id: string;
  stream: "stdout" | "stderr";
  line: string;
}

export interface DoneEvent {
  run_id: string;
  exit_code: number | null;
  cancelled: boolean;
  profile?: RunProfile;
}

export interface StepTiming {
  wall_ms: number;
  // Any other fields the profile carries (e.g. n_genes, cpu_ms).
  [extra: string]: unknown;
}

export interface RunProfile {
  steps: Record<string, StepTiming>;
  metadata: Record<string, unknown>;
  total_wall_ms: number;
}

export type RunStatus =
  | { kind: "idle" }
  | { kind: "running"; run_id: string; started_at: number }
  | {
      kind: "done";
      run_id: string;
      exit_code: number | null;
      duration_ms: number;
      profile?: RunProfile;
    }
  | { kind: "cancelled"; run_id: string; duration_ms: number }
  | { kind: "failed"; run_id: string; message: string };
