import { isTauriContext } from "../lib/tauri";

export function TauriRequiredBanner() {
  if (isTauriContext()) return null;
  return (
    <div
      style={{
        padding: "8px 16px",
        background: "rgba(213, 94, 0, 0.15)",
        borderBottom: "1px solid rgba(213, 94, 0, 0.4)",
        fontSize: 12,
        color: "rgb(180, 80, 0)",
      }}
    >
      <strong>Running in browser-only mode.</strong>{" "}
      File open / save and CSV loading require the Tauri runtime. Restart
      with <code>npm run tauri:dev</code> to enable them.
    </div>
  );
}
