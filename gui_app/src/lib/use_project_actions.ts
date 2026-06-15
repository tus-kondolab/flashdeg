// Project file actions (New / Open / Save / Save As), shared by the native
// menu and any in-app buttons. These act on the CURRENT project; the caller is
// responsible for the unsaved-changes guard (see use_discard_guard) before a
// discarding action (New / Open). Save / Save As report success as a boolean so
// the guard can decide whether to proceed after "Save & continue".

import { useCallback, useState } from "react";
import { open, save as saveDialog } from "@tauri-apps/plugin-dialog";
import { useAppState } from "./project_context";
import { loadProject, saveProject } from "./tauri";

const PROJECT_FILTER = [{ name: "FlashDEG project", extensions: ["flashdeg"] }];

export interface ProjectActions {
  doOpen: () => Promise<void>;
  /** Save to the current path (prompting if none). Resolves true when written. */
  save: () => Promise<boolean>;
  /** Always prompt for a new path. Resolves true when written. */
  saveAs: () => Promise<boolean>;
  error: string | null;
  clearError: () => void;
}

function baseName(path: string): string {
  const sep = path.includes("\\") ? "\\" : "/";
  const i = path.lastIndexOf(sep);
  return i >= 0 ? path.slice(i + 1) : path;
}

function formatError(e: unknown): string {
  if (typeof e === "string") return e;
  if (e instanceof Error) return e.message;
  return JSON.stringify(e);
}

export function useProjectActions(): ProjectActions {
  const { project, projectPath, setProject, markProjectSaved } = useAppState();
  const [error, setError] = useState<string | null>(null);

  const doOpen = useCallback(async () => {
    setError(null);
    try {
      const picked = await open({ multiple: false, directory: false, filters: PROJECT_FILTER });
      if (!picked) return;
      const path = Array.isArray(picked) ? picked[0] : picked;
      const loaded = await loadProject(path);
      setProject(loaded, path);
    } catch (e) {
      setError(formatError(e));
    }
  }, [setProject]);

  const save = useCallback(async (): Promise<boolean> => {
    setError(null);
    try {
      let path = projectPath;
      if (!path) {
        const picked = await saveDialog({ defaultPath: "untitled.flashdeg", filters: PROJECT_FILTER });
        if (!picked) return false;
        path = picked;
      }
      await saveProject(path, project);
      if (path !== projectPath) setProject(project, path);
      else markProjectSaved();
      return true;
    } catch (e) {
      setError(formatError(e));
      return false;
    }
  }, [project, projectPath, setProject, markProjectSaved]);

  const saveAs = useCallback(async (): Promise<boolean> => {
    setError(null);
    try {
      const defaultPath = projectPath ? baseName(projectPath) : "untitled.flashdeg";
      const picked = await saveDialog({ defaultPath, filters: PROJECT_FILTER });
      if (!picked) return false;
      await saveProject(picked, project);
      setProject(project, picked);
      return true;
    } catch (e) {
      setError(formatError(e));
      return false;
    }
  }, [project, projectPath, setProject]);

  const clearError = useCallback(() => setError(null), []);

  return { doOpen, save, saveAs, error, clearError };
}
