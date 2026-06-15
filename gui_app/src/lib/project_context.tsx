// Application-wide state. The project is ALWAYS present (an untitled project
// is created on launch) so every screen can edit it as the single source of
// truth — no `project | null` guards, no per-screen sandbox fields. See
// gui_ux_improvements.md § 3.1.
//
// Binary path and thread count are app-level (not per-project) settings,
// persisted to localStorage.

import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { newDefaultProject, type Project } from "./project";
import {
  defaultThreads,
  loadBinaryPath,
  saveBinaryPath,
  loadGeneMapPath,
  saveGeneMapPath,
  type GeneMapSlot,
} from "./app_settings";
import { isTauriContext, locateFlashdeg } from "./tauri";

export type AppTab = "data" | "analyze" | "results";

interface AppStateValue {
  project: Project;
  projectPath: string | null;
  projectDirty: boolean;
  /** Bumped when the whole project object is replaced (New / Open), but NOT on
   *  in-place edits or a path-only update (first save of an untitled project).
   *  Screens key "reset my view state" effects on this so a replacement always
   *  resets even when the path is unchanged (e.g. New from an untitled project). */
  projectEpoch: number;
  /** Replace the whole project (Open / New). Clears the dirty flag. */
  setProject(project: Project, path: string | null): void;
  /** Apply an in-memory mutation. Marks the project dirty. */
  updateProject(mutator: (project: Project) => Project): void;
  /** Clear the dirty flag after a successful disk save. */
  markProjectSaved(): void;

  // App-level settings (not part of the project file).
  binaryPath: string;
  setBinaryPath(path: string): void;
  threads: number;
  setThreads(n: number): void;
  // Optional per-slot gene-symbol map files (Results display). Species is always
  // auto-detected; these override the bundled human/fly maps and supply a custom
  // map for other IDs. Empty string = not set. Persisted.
  geneMapPaths: Record<GeneMapSlot, string>;
  setGeneMapPath(slot: GeneMapSlot, path: string): void;

  pendingResultsPath: string | null;
  setPendingResultsPath(path: string | null): void;

  navigateTo: (tab: AppTab) => void;
  registerNavigate(fn: (tab: AppTab) => void): void;
}

const AppStateContext = createContext<AppStateValue | null>(null);

export function AppStateProvider({ children }: { children: ReactNode }) {
  const [project, setProjectState] = useState<Project>(() => newDefaultProject());
  const [projectPath, setProjectPath] = useState<string | null>(null);
  const [projectDirty, setProjectDirty] = useState<boolean>(false);
  const [projectEpoch, setProjectEpoch] = useState<number>(0);
  const [binaryPath, setBinaryPathState] = useState<string>(() => loadBinaryPath());
  const [geneMapPaths, setGeneMapPathsState] = useState<Record<GeneMapSlot, string>>(() => ({
    human: loadGeneMapPath("human"),
    fly: loadGeneMapPath("fly"),
    custom: loadGeneMapPath("custom"),
  }));
  // Threads are NOT persisted: default each launch, reset on New.
  const [threads, setThreadsState] = useState<number>(() => defaultThreads());
  const [pendingResultsPath, setPendingResultsPath] = useState<string | null>(null);
  const [navigateImpl, setNavigateImpl] = useState<((tab: AppTab) => void) | null>(null);

  // Track the live project so setProject can tell a genuine replacement
  // (New / Open — a new object) from a path-only update (first save of an
  // untitled project reuses the same object) and only bump the epoch for the
  // former.
  const projectRef = useRef(project);
  projectRef.current = project;

  const setProject = useCallback((p: Project, path: string | null) => {
    if (p !== projectRef.current) setProjectEpoch((e) => e + 1);
    setProjectState(p);
    setProjectPath(path);
    setProjectDirty(false);
  }, []);

  const updateProject = useCallback((mutator: (project: Project) => Project) => {
    setProjectState((prev) => {
      const next = mutator(prev);
      if (next !== prev) setProjectDirty(true);
      return next;
    });
  }, []);

  const markProjectSaved = useCallback(() => setProjectDirty(false), []);

  const setBinaryPath = useCallback((p: string) => {
    setBinaryPathState(p);
    saveBinaryPath(p);
  }, []);

  const setGeneMapPath = useCallback((slot: GeneMapSlot, p: string) => {
    setGeneMapPathsState((prev) => ({ ...prev, [slot]: p }));
    saveGeneMapPath(slot, p);
  }, []);

  // Auto-locate flashdeg each launch (see paths.rs): a binary co-located with
  // the GUI wins, then PATH, then the parent / .app-container folder. Not
  // persisted, so a manual localStorage override is only used when none is
  // found. Re-derived on every launch.
  useEffect(() => {
    if (!isTauriContext()) return;
    locateFlashdeg()
      .then((p) => {
        if (p) setBinaryPathState(p);
      })
      .catch(() => {
        // no PATH/bundled binary; keep whatever localStorage gave us
      });
  }, []);

  const setThreads = useCallback((n: number) => {
    const v = Number.isFinite(n) && n >= 1 ? Math.floor(n) : 1;
    setThreadsState(v);
  }, []);

  const navigateTo = useCallback(
    (tab: AppTab) => navigateImpl?.(tab),
    [navigateImpl],
  );

  const registerNavigate = useCallback((fn: (tab: AppTab) => void) => {
    setNavigateImpl(() => fn);
  }, []);

  const value = useMemo<AppStateValue>(
    () => ({
      project,
      projectPath,
      projectDirty,
      projectEpoch,
      setProject,
      updateProject,
      markProjectSaved,
      binaryPath,
      setBinaryPath,
      threads,
      setThreads,
      geneMapPaths,
      setGeneMapPath,
      pendingResultsPath,
      setPendingResultsPath,
      navigateTo,
      registerNavigate,
    }),
    [
      project,
      projectPath,
      projectDirty,
      projectEpoch,
      binaryPath,
      threads,
      geneMapPaths,
      pendingResultsPath,
      navigateTo,
      registerNavigate,
      setProject,
      updateProject,
      markProjectSaved,
      setBinaryPath,
      setThreads,
      setGeneMapPath,
    ],
  );

  return <AppStateContext.Provider value={value}>{children}</AppStateContext.Provider>;
}

export function useAppState(): AppStateValue {
  const ctx = useContext(AppStateContext);
  if (!ctx) {
    throw new Error("useAppState must be used within an AppStateProvider");
  }
  return ctx;
}
