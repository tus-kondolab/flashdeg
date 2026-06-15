// Typed wrappers over Tauri IPC commands. Names must match the
// `#[tauri::command]` functions in src-tauri/src/commands.rs.

import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { AnalysisRequest, DoneEvent, LogEvent, StartedEvent } from "./analysis";
import type {
  FeatureCountsInspection,
  MergeReport,
  MergeRequest,
  WriteMetadataReport,
  WriteMetadataRequest,
} from "./featurecounts";
import type { Project } from "./project";
import type { ResultsTable } from "./results";
import type {
  MetadataSummary,
  ValidationRequest,
  ValidationResult,
} from "./validation";

export function isTauriContext(): boolean {
  return (
    typeof window !== "undefined" &&
    typeof (window as unknown as { __TAURI_INTERNALS__?: unknown })
      .__TAURI_INTERNALS__ !== "undefined"
  );
}

function requireTauri(): void {
  if (!isTauriContext()) {
    throw new Error(
      "This action requires the Tauri desktop runtime. " +
        "Launch the app with `npm run tauri:dev` (or run the packaged " +
        "binary) instead of `npm run dev`.",
    );
  }
}

export async function loadProject(path: string): Promise<Project> {
  requireTauri();
  return invoke<Project>("load_project", { path });
}

export async function saveProject(path: string, project: Project): Promise<void> {
  requireTauri();
  await invoke<void>("save_project", { path, project });
}

export async function defaultProject(): Promise<Project> {
  requireTauri();
  return invoke<Project>("default_project");
}

export async function loadResultsCsv(path: string): Promise<ResultsTable> {
  requireTauri();
  return invoke<ResultsTable>("load_results_csv", { path });
}

export async function validateInputs(
  request: ValidationRequest,
): Promise<ValidationResult> {
  requireTauri();
  return invoke<ValidationResult>("validate_inputs", { request });
}

export async function inspectMetadata(path: string): Promise<MetadataSummary> {
  requireTauri();
  return invoke<MetadataSummary>("inspect_metadata", { path });
}

/** PATH-resolved (or bundled) flashdeg, or null if none found. */
export async function locateFlashdeg(): Promise<string | null> {
  requireTauri();
  return invoke<string | null>("locate_flashdeg");
}

/** Launch a new, independent instance of the GUI (File ▸ New Window). */
export async function launchNewInstance(): Promise<void> {
  requireTauri();
  await invoke<void>("launch_new_instance");
}

/** Raw `.tsv.gz` bytes of the external gene-ID→symbol map for a species, read
 *  from the `gene_maps/` folder next to the app. Rejects when the file is
 *  absent (caller treats that as "no symbols"). */
export async function readGeneMap(species: string): Promise<ArrayBuffer> {
  requireTauri();
  return invoke<ArrayBuffer>("read_gene_map", { species });
}

/** Raw bytes of a user-supplied gene-ID→symbol map at an arbitrary path (the
 *  "Custom" gene map). `.tsv` or `.tsv.gz`. Rejects when the file is missing. */
export async function readGeneMapFile(path: string): Promise<ArrayBuffer> {
  requireTauri();
  return invoke<ArrayBuffer>("read_gene_map_file", { path });
}

/** Write exported plot image bytes to an absolute path (parents created). */
export async function savePlot(path: string, bytes: Uint8Array): Promise<void> {
  requireTauri();
  await invoke<void>("save_plot", { path, bytes: Array.from(bytes) });
}

/** Put an SVG on the clipboard as a vector graphic (image/svg+xml). */
export async function copySvg(svg: string): Promise<void> {
  requireTauri();
  await invoke<void>("copy_svg", { svg });
}

/** Tokenize an extra-args string the way the run will (shell-words, no shell).
 *  Resolves to the argv tokens, or rejects with the parse error message. */
export async function validateExtraArgs(text: string): Promise<string[]> {
  requireTauri();
  return invoke<string[]>("validate_extra_args", { text });
}

export async function runAnalysis(request: AnalysisRequest): Promise<string> {
  requireTauri();
  return invoke<string>("run_analysis", { request });
}

export async function cancelAnalysis(runId: string): Promise<void> {
  requireTauri();
  await invoke<void>("cancel_analysis", { runId });
}

export async function inspectFeatureCounts(
  path: string,
): Promise<FeatureCountsInspection> {
  requireTauri();
  return invoke<FeatureCountsInspection>("inspect_featurecounts", { path });
}

export async function mergeFeatureCounts(
  request: MergeRequest,
): Promise<MergeReport> {
  requireTauri();
  return invoke<MergeReport>("merge_featurecounts", { request });
}

export async function writeMetadataCsv(
  request: WriteMetadataRequest,
): Promise<WriteMetadataReport> {
  requireTauri();
  return invoke<WriteMetadataReport>("write_metadata_csv", { request });
}

/**
 * Subscribe to all FlashDEG run events. Returns an unlisten function that
 * removes all three listeners. Callers should call this once per page mount
 * and keep the unlisten in a useEffect cleanup.
 */
export async function listenToRun(handlers: {
  onStarted?: (e: StartedEvent) => void;
  onLog?: (e: LogEvent) => void;
  onDone?: (e: DoneEvent) => void;
}): Promise<UnlistenFn> {
  requireTauri();
  const unlisteners: UnlistenFn[] = [];
  if (handlers.onStarted) {
    unlisteners.push(
      await listen<StartedEvent>("flashdeg:started", (e) =>
        handlers.onStarted!(e.payload),
      ),
    );
  }
  if (handlers.onLog) {
    unlisteners.push(
      await listen<LogEvent>("flashdeg:log", (e) => handlers.onLog!(e.payload)),
    );
  }
  if (handlers.onDone) {
    unlisteners.push(
      await listen<DoneEvent>("flashdeg:done", (e) => handlers.onDone!(e.payload)),
    );
  }
  return () => {
    unlisteners.forEach((u) => u());
  };
}
