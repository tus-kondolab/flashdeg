// Renders FlashDEG's profile.json as a stage-by-stage timing breakdown.
// See gui_plan.md § 9.

import type { RunProfile } from "../lib/analysis";

interface DisplayStep {
  name: string;
  wall_ms: number;
  fraction: number;
}

export function ProfileSummary({ profile }: { profile: RunProfile }) {
  const steps = collectSteps(profile);
  const total = profile.total_wall_ms;
  const flashdegVersion = readString(profile.metadata, "flashdeg_version");
  const gitRevision = readString(profile.metadata, "git_revision");

  return (
    <div
      style={{
        border: "1px solid rgba(127,127,127,0.25)",
        borderRadius: 6,
        padding: 10,
        fontSize: 11,
      }}
    >
      <div style={{ display: "flex", alignItems: "baseline", gap: 12, marginBottom: 8 }}>
        <strong style={{ fontSize: 13 }}>Pipeline timing</strong>
        <span style={{ opacity: 0.7 }}>
          total <code>{formatMs(total)}</code>
          {steps.length > 0 && ` over ${steps.length} stages`}
        </span>
        {(flashdegVersion || gitRevision) && (
          <span style={{ opacity: 0.6 }}>
            {flashdegVersion && `flashdeg ${flashdegVersion}`}
            {flashdegVersion && gitRevision && " · "}
            {gitRevision && `git ${gitRevision}`}
          </span>
        )}
      </div>
      {steps.length === 0 ? (
        <div style={{ opacity: 0.6 }}>No stage timings recorded.</div>
      ) : (
        <table style={{ width: "100%", borderCollapse: "collapse" }}>
          <tbody>
            {steps.map((s) => (
              <tr key={s.name}>
                <td style={{ padding: "2px 6px", whiteSpace: "nowrap", fontFamily: "var(--mono)" }}>
                  {prettifyName(s.name)}
                </td>
                <td style={{ padding: "2px 6px", width: "60%" }}>
                  <div
                    style={{
                      height: 8,
                      background: "rgba(127,127,127,0.15)",
                      borderRadius: 4,
                      overflow: "hidden",
                    }}
                  >
                    <div
                      style={{
                        height: "100%",
                        width: `${Math.max(s.fraction * 100, 1)}%`,
                        background: "rgb(0, 114, 178)",
                      }}
                    />
                  </div>
                </td>
                <td style={{ padding: "2px 6px", textAlign: "right", fontFamily: "var(--mono)" }}>
                  {formatMs(s.wall_ms)}
                </td>
                <td style={{ padding: "2px 6px", textAlign: "right", opacity: 0.7, width: 50 }}>
                  {(s.fraction * 100).toFixed(1)}%
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  );
}

function collectSteps(profile: RunProfile): DisplayStep[] {
  const entries = Object.entries(profile.steps)
    .map(([name, t]) => ({ name, wall_ms: t.wall_ms ?? 0 }))
    .filter((s) => s.wall_ms > 0)
    .sort((a, b) => b.wall_ms - a.wall_ms);
  const total = entries.reduce((acc, s) => acc + s.wall_ms, 0);
  return entries.map((s) => ({
    ...s,
    fraction: total > 0 ? s.wall_ms / total : 0,
  }));
}

function formatMs(ms: number): string {
  if (ms >= 1000) return `${(ms / 1000).toFixed(2)}s`;
  return `${ms.toFixed(0)}ms`;
}

function prettifyName(name: string): string {
  return name.replace(/_ms$/, "").replace(/_/g, " ");
}

function readString(meta: Record<string, unknown>, key: string): string | null {
  const v = meta[key];
  return typeof v === "string" ? v : null;
}
