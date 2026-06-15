import { useEffect, useRef, useState } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { DataPage } from "./pages/DataPage";
import { AnalyzePage } from "./pages/AnalyzePage";
import { ResultsPage } from "./pages/ResultsPage";
import { TauriRequiredBanner } from "./components/TauriRequiredBanner";
import { RunHistoryPanel } from "./components/RunHistoryPanel";
import { PreferencesModal } from "./components/PreferencesModal";
import { AppStateProvider, useAppState, type AppTab } from "./lib/project_context";
import { useProjectActions } from "./lib/use_project_actions";
import { useDiscardGuard } from "./lib/use_discard_guard";
import { useAppMenu, useMenuShortcuts } from "./lib/use_app_menu";
import { isTauriContext, launchNewInstance } from "./lib/tauri";

const STEPS: { id: AppTab; n: number; label: string }[] = [
  { id: "data", n: 1, label: "Data" },
  { id: "analyze", n: 2, label: "Analyze" },
  { id: "results", n: 3, label: "Results" },
];

export function App() {
  return (
    <AppStateProvider>
      <AppShell />
    </AppStateProvider>
  );
}

function AppShell() {
  const [tab, setTab] = useState<AppTab>("data");
  const [historyOpen, setHistoryOpen] = useState(false);
  const [prefsOpen, setPrefsOpen] = useState(false);
  const { registerNavigate, project, projectPath, projectDirty, updateProject, setThreads, setPendingResultsPath, navigateTo } =
    useAppState();

  useEffect(() => {
    registerNavigate((t: AppTab) => setTab(t));
  }, [registerNavigate]);

  // Show "FlashDEG — <project> [•]" in the native title bar instead of an
  // in-app bar (• marks unsaved changes). A tab strip can go here later.
  useEffect(() => {
    if (!isTauriContext()) return;
    const name = projectPath ? baseName(projectPath) : "untitled";
    const title = `FlashDEG — ${name}${projectDirty ? " •" : ""}`;
    getCurrentWindow().setTitle(title).catch(() => {});
  }, [projectPath, projectDirty]);

  // Project file actions + unsaved-changes guard, driven by the native menu.
  const actions = useProjectActions();
  const dirtyRef = useRef(projectDirty);
  dirtyRef.current = projectDirty;
  const { confirmDiscard, modal } = useDiscardGuard({
    isDirty: () => dirtyRef.current,
    save: actions.save,
  });
  const confirmRef = useRef(confirmDiscard);
  confirmRef.current = confirmDiscard;

  const guardedOpen = async () => { if (await confirmDiscard()) await actions.doOpen(); };
  const quit = async () => { if (await confirmDiscard()) await getCurrentWindow().destroy(); };

  // Intercept the window close (X) so unsaved work prompts the same guard.
  useEffect(() => {
    if (!isTauriContext()) return;
    let unlisten: (() => void) | undefined;
    getCurrentWindow()
      .onCloseRequested(async (e) => {
        e.preventDefault();
        if (await confirmRef.current()) await getCurrentWindow().destroy();
      })
      .then((u) => { unlisten = u; });
    return () => unlisten?.();
  }, []);

  // Esc closes the Run history drawer.
  useEffect(() => {
    if (!historyOpen) return;
    const onKey = (e: KeyboardEvent) => { if (e.key === "Escape") setHistoryOpen(false); };
    document.addEventListener("keydown", onKey);
    return () => document.removeEventListener("keydown", onKey);
  }, [historyOpen]);

  const menuActions = {
    openNewWindow: () => { launchNewInstance().catch((e) => console.error("New Window failed:", e)); },
    openProject: guardedOpen,
    save: actions.save,
    saveAs: actions.saveAs,
    openPreferences: () => setPrefsOpen(true),
    quit,
    toggleHistory: () => setHistoryOpen((v) => !v),
  };
  useAppMenu(menuActions);
  useMenuShortcuts(menuActions); // JS fallback for File shortcuts (WebView eats native ones)

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%" }}>
      <TauriRequiredBanner />
      {actions.error && (
        <div style={{ background: "rgba(176,0,32,0.1)", color: "#b00020", fontSize: 12, padding: "4px 16px", display: "flex", gap: 10 }}>
          <span style={{ flex: 1 }}>{actions.error}</span>
          <button onClick={actions.clearError} style={{ fontSize: 11, padding: "0 6px" }}>dismiss</button>
        </div>
      )}
      {modal}

      {prefsOpen && <PreferencesModal onClose={() => setPrefsOpen(false)} />}

      {historyOpen && (
        <>
          {/* Click-outside backdrop. */}
          <div
            onClick={() => setHistoryOpen(false)}
            style={{ position: "fixed", inset: 0, background: "rgba(0,0,0,0.2)", zIndex: 40 }}
          />
          {/* Right-side drawer (overlays content; Esc / X / backdrop to close). */}
          <aside
            style={{
              position: "fixed", top: 0, right: 0, bottom: 0, width: 380, maxWidth: "90vw",
              background: "#fff", borderLeft: "1px solid rgba(127,127,127,0.3)",
              boxShadow: "-6px 0 24px rgba(0,0,0,0.18)", zIndex: 41,
              display: "flex", flexDirection: "column",
            }}
          >
            <div style={{ display: "flex", alignItems: "center", gap: 8, padding: "10px 12px", borderBottom: "1px solid rgba(127,127,127,0.2)" }}>
              <strong style={{ fontSize: 13, flex: 1 }}>
                Run history <span style={{ fontWeight: 400, opacity: 0.6, fontSize: 11 }}>{project.runs.length} run(s)</span>
              </strong>
              <button onClick={() => setHistoryOpen(false)} title="Close (Esc)" style={{ fontSize: 16, lineHeight: 1, padding: "2px 8px" }}>×</button>
            </div>
            <div style={{ flex: 1, minHeight: 0, overflow: "auto", padding: "8px 12px" }}>
              <RunHistoryPanel
                runs={project.runs}
                onOpenInResults={(path) => {
                  setPendingResultsPath(path);
                  navigateTo("results");
                  setHistoryOpen(false);
                }}
                onRemove={(index) =>
                  updateProject((prev) => ({ ...prev, runs: prev.runs.filter((_, i) => i !== index) }))
                }
                onRestore={(p) => {
                  updateProject((prev) => ({
                    ...prev,
                    inputs: p.inputs,
                    design: p.design,
                    contrast: p.contrast,
                    ref_levels: p.ref_levels,
                    options: p.options,
                  }));
                  setThreads(p.threads);
                  navigateTo("analyze");
                  setHistoryOpen(false);
                }}
              />
              <details style={{ marginTop: 8 }}>
                <summary style={{ cursor: "pointer", fontSize: 11, opacity: 0.7 }}>
                  Project file (raw JSON)
                </summary>
                <pre
                  style={{
                    margin: "6px 0 0",
                    padding: 10,
                    background: "rgba(127,127,127,0.08)",
                    borderRadius: 6,
                    overflow: "auto",
                    maxHeight: 200,
                    fontSize: 11,
                  }}
                >
                  {JSON.stringify(project, null, 2)}
                </pre>
              </details>
            </div>
          </aside>
        </>
      )}

      <nav
        style={{
          display: "flex",
          alignItems: "center",
          gap: 4,
          padding: "6px 16px",
          borderBottom: "1px solid rgba(127,127,127,0.25)",
        }}
      >
        {STEPS.map((s, i) => (
          <div key={s.id} style={{ display: "flex", alignItems: "center" }}>
            <button
              onClick={() => setTab(s.id)}
              style={{
                display: "flex",
                alignItems: "center",
                gap: 6,
                borderColor: tab === s.id ? "#0072B2" : "transparent",
                background: tab === s.id ? "rgba(0,114,178,0.08)" : "transparent",
                fontWeight: tab === s.id ? 600 : 400,
              }}
            >
              <span
                style={{
                  fontSize: 11,
                  fontWeight: 700,
                  color: tab === s.id ? "#0072B2" : "#999",
                  border: `1px solid ${tab === s.id ? "#0072B2" : "#bbb"}`,
                  borderRadius: 10,
                  padding: "0 7px",
                }}
              >
                {s.n}
              </span>
              {s.label}
            </button>
            {i < STEPS.length - 1 && <span style={{ opacity: 0.3, margin: "0 2px" }}>→</span>}
          </div>
        ))}
      </nav>

      {/* All three steps stay mounted; we toggle visibility so loaded
          results, run logs, filters, and selections survive tab switches.
          Data is only re-computed when the user explicitly re-runs.
          (gui_ux_improvements item: keep results across navigation) */}
      <main style={{ flex: 1, minHeight: 0, position: "relative" }}>
        <TabPanel active={tab === "data"}><DataPage /></TabPanel>
        <TabPanel active={tab === "analyze"}><AnalyzePage /></TabPanel>
        <TabPanel active={tab === "results"}><ResultsPage /></TabPanel>
      </main>
    </div>
  );
}

function TabPanel({ active, children }: { active: boolean; children: React.ReactNode }) {
  return <div style={{ height: "100%", display: active ? "block" : "none" }}>{children}</div>;
}

function baseName(path: string): string {
  const sep = path.includes("\\") ? "\\" : "/";
  const i = path.lastIndexOf(sep);
  return i >= 0 ? path.slice(i + 1) : path;
}
