// App-level settings that are NOT part of a project file. The FlashDEG binary
// location and the gene-symbol map preference persist (both are painful to
// re-enter). The thread count is NOT persisted: it starts at the machine
// default each launch and resets on New, so a previous session's value never
// silently carries over.

const BINARY_KEY = "flashdeg.binaryPath";

export function loadBinaryPath(): string {
  try {
    return localStorage.getItem(BINARY_KEY) ?? "";
  } catch {
    return "";
  }
}

export function saveBinaryPath(path: string): void {
  try {
    localStorage.setItem(BINARY_KEY, path);
  } catch {
    // ignore (private mode / disabled storage)
  }
}

// Gene-symbol map files (Results view, display only). The species is always
// auto-detected from the gene_id; these are optional user-supplied
// gene_id->symbol files per slot:
//   human / fly  — override the built-in bundled map when set;
//   custom       — used when the gene_id is neither ENSG (human) nor FBgn (fly).
// Empty string = not set (use the bundled map for human/fly, none for custom).
export type GeneMapSlot = "human" | "fly" | "custom";

const GENE_MAP_PATH_KEYS: Record<GeneMapSlot, string> = {
  human: "flashdeg.geneMapHumanPath",
  fly: "flashdeg.geneMapFlyPath",
  custom: "flashdeg.geneMapCustomPath",
};

export function loadGeneMapPath(slot: GeneMapSlot): string {
  try {
    return localStorage.getItem(GENE_MAP_PATH_KEYS[slot]) ?? "";
  } catch {
    return "";
  }
}

export function saveGeneMapPath(slot: GeneMapSlot, path: string): void {
  try {
    localStorage.setItem(GENE_MAP_PATH_KEYS[slot], path);
  } catch {
    // ignore
  }
}

/** Default worker threads: half the logical CPUs (rounded down, min 1), so a
 *  run leaves headroom for the rest of the system. Falls back to 4 when the
 *  browser doesn't expose a core count. */
export function defaultThreads(): number {
  const n = typeof navigator !== "undefined" ? navigator.hardwareConcurrency : 0;
  return n && n > 0 ? Math.max(1, Math.floor(n / 2)) : 4;
}
