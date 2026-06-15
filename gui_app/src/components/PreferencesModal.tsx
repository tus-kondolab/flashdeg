// App-level Preferences (File ▸ Preferences…, Ctrl/Cmd+,). Holds settings that
// are NOT part of a project file: the FlashDEG engine binary, and the gene
// symbol maps used in the Results view. All persist via project_context /
// app_settings (localStorage). Display-only; nothing here changes analysis
// inputs or the project.
//
// Gene symbols: the species is ALWAYS auto-detected from the gene_id
// (ENSG → human, FBgn → fly). The three file slots are optional overrides —
// human/fly replace the built-in bundled map, and custom is used when the IDs
// are neither ENSG nor FBgn.

import { Fragment } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { useAppState } from "../lib/project_context";
import type { GeneMapSlot } from "../lib/app_settings";

// Binary picker filter: restrict to `.exe` on Windows only. On macOS/Linux the
// FlashDEG binary is extensionless (`flashdeg`), and a native open dialog with
// an extension filter greys out extensionless files — so use no filter there.
function binaryDialogFilters(): { name: string; extensions: string[] }[] | undefined {
  const isWindows = typeof navigator !== "undefined" && /Windows/i.test(navigator.userAgent);
  return isWindows ? [{ name: "Executable", extensions: ["exe"] }] : undefined;
}

const GENE_MAP_FILTER = [{ name: "Gene map (TSV)", extensions: ["tsv", "gz", "txt"] }];

const GENE_MAP_ROWS: { slot: GeneMapSlot; title: string; placeholder: string }[] = [
  { slot: "human", title: "Homo sapiens (ENSG)", placeholder: "built-in" },
  { slot: "fly", title: "Drosophila melanogaster (FBgn)", placeholder: "built-in" },
  { slot: "custom", title: "Custom (other IDs)", placeholder: "none" },
];

const pathBoxStyle = {
  border: "1px solid rgba(127,127,127,0.4)", borderRadius: 4, padding: "5px 8px",
  fontSize: 12, wordBreak: "break-all" as const, background: "#fff",
};

export function PreferencesModal({ onClose }: { onClose: () => void }) {
  const { binaryPath, setBinaryPath, geneMapPaths, setGeneMapPath } = useAppState();

  const pickBinary = async () => {
    const picked = await open({ multiple: false, directory: false, filters: binaryDialogFilters() });
    if (picked) setBinaryPath(Array.isArray(picked) ? picked[0] : picked);
  };

  const pickMap = async (slot: GeneMapSlot) => {
    const picked = await open({ multiple: false, directory: false, filters: GENE_MAP_FILTER });
    if (picked) setGeneMapPath(slot, Array.isArray(picked) ? picked[0] : picked);
  };

  return (
    <div
      role="dialog"
      aria-modal="true"
      onKeyDown={(e) => { if (e.key === "Escape") onClose(); }}
      style={{
        position: "fixed", inset: 0, zIndex: 1000,
        background: "rgba(0,0,0,0.35)",
        display: "flex", alignItems: "center", justifyContent: "center",
      }}
      onClick={(e) => { if (e.target === e.currentTarget) onClose(); }}
    >
      <div
        style={{
          background: "#fff", color: "#111", borderRadius: 8, padding: 20, width: 560, maxWidth: "92vw",
          boxShadow: "0 10px 40px rgba(0,0,0,0.3)", fontSize: 13,
        }}
      >
        <div style={{ display: "flex", alignItems: "center", marginBottom: 14 }}>
          <strong style={{ fontSize: 15, flex: 1 }}>Preferences</strong>
          <button onClick={onClose} title="Close (Esc)" style={{ fontSize: 16, lineHeight: 1, padding: "2px 8px" }}>×</button>
        </div>

        {/* FlashDEG engine binary. */}
        <section style={{ marginBottom: 18 }}>
          <div style={{ fontWeight: 600, marginBottom: 6 }}>FlashDEG binary</div>
          <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
            <code style={{ flex: 1, ...pathBoxStyle, opacity: binaryPath ? 0.9 : 0.45 }}>
              {binaryPath || "(not set)"}
            </code>
            <button onClick={pickBinary}>Browse…</button>
          </div>
        </section>

        {/* Gene symbol maps (Results view). Species is always auto-detected. */}
        <section>
          <div style={{ fontWeight: 600, marginBottom: 4 }}>Gene symbols</div>
          <div style={{ fontSize: 11, opacity: 0.6, marginBottom: 10 }}>
            Species is auto-detected from gene IDs. Optionally override with your own
            <code> gene_id ⇥ symbol</code> file (<code>.tsv</code> / <code>.tsv.gz</code>).
          </div>
          <div style={{ display: "grid", gridTemplateColumns: "auto 1fr auto", gap: "8px 10px", alignItems: "center" }}>
            {GENE_MAP_ROWS.map((row) => (
              <Fragment key={row.slot}>
                <span style={{ whiteSpace: "nowrap" }}>{row.title}</span>
                <code style={{ ...pathBoxStyle, opacity: geneMapPaths[row.slot] ? 0.9 : 0.45 }}>
                  {geneMapPaths[row.slot] || row.placeholder}
                </code>
                <button onClick={() => pickMap(row.slot)}>Browse…</button>
              </Fragment>
            ))}
          </div>
        </section>

        <div style={{ display: "flex", justifyContent: "flex-end", marginTop: 20 }}>
          <button onClick={onClose} style={{ padding: "4px 14px", fontWeight: 600, background: "#0072B2", color: "#fff", border: "1px solid #0072B2", borderRadius: 4 }}>
            Done
          </button>
        </div>
      </div>
    </div>
  );
}
