// Unsaved-changes guard, shared by discarding actions (New / Open) and by app
// quit / window close. `confirmDiscard()` resolves true when it's safe to
// proceed (discard the current project): immediately when not dirty, otherwise
// after the user answers a 3-way modal (Save & continue / Discard / Cancel).

import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";

interface GuardOpts {
  /** Latest dirty flag. */
  isDirty: () => boolean;
  /** Save the current project; resolves true when written (false = cancelled). */
  save: () => Promise<boolean>;
}

export function useDiscardGuard(opts: GuardOpts): {
  confirmDiscard: () => Promise<boolean>;
  modal: ReactNode;
} {
  const optsRef = useRef(opts);
  optsRef.current = opts;
  const [pending, setPending] = useState<{ resolve: (v: boolean) => void } | null>(null);

  const confirmDiscard = useCallback((): Promise<boolean> => {
    if (!optsRef.current.isDirty()) return Promise.resolve(true);
    return new Promise<boolean>((resolve) => setPending({ resolve }));
  }, []);

  const choose = useCallback(
    async (choice: "save" | "discard" | "cancel") => {
      const p = pending;
      if (!p) return;
      if (choice === "cancel") {
        setPending(null);
        p.resolve(false);
        return;
      }
      if (choice === "discard") {
        setPending(null);
        p.resolve(true);
        return;
      }
      // Save & continue: only proceed if the save actually completed.
      const ok = await optsRef.current.save();
      setPending(null);
      p.resolve(ok);
    },
    [pending],
  );

  const modal = pending ? <UnsavedModal onChoose={choose} /> : null;
  return { confirmDiscard, modal };
}

function UnsavedModal({ onChoose }: { onChoose: (c: "save" | "discard" | "cancel") => void }) {
  // Visual left-to-right order; the index also drives ←/→ arrow-key navigation.
  const order = ["cancel", "discard", "save"] as const;
  const labels: Record<(typeof order)[number], string> = {
    cancel: "Cancel",
    discard: "Don’t save",
    save: "Save",
  };
  const btnRefs = useRef<Array<HTMLButtonElement | null>>([]);
  // Default selection: Save (rightmost), matching the previous autoFocus.
  const [focusIdx, setFocusIdx] = useState(order.length - 1);

  // Keep the real DOM focus on the selected button so Enter/Space activate it.
  useEffect(() => {
    btnRefs.current[focusIdx]?.focus();
  }, [focusIdx]);

  return (
    <div
      role="dialog"
      aria-modal="true"
      onKeyDown={(e) => {
        if (e.key === "Escape") { onChoose("cancel"); return; }
        if (e.key === "ArrowLeft" || e.key === "ArrowRight") {
          e.preventDefault();
          const delta = e.key === "ArrowLeft" ? -1 : 1;
          setFocusIdx((i) => (i + delta + order.length) % order.length);
        }
      }}
      style={{
        position: "fixed", inset: 0, zIndex: 1000,
        background: "rgba(0,0,0,0.35)",
        display: "flex", alignItems: "center", justifyContent: "center",
      }}
    >
      <div
        style={{
          background: "#fff", color: "#111", borderRadius: 8, padding: 20, width: 380,
          boxShadow: "0 10px 40px rgba(0,0,0,0.3)", fontSize: 13,
        }}
      >
        <div style={{ fontWeight: 600, marginBottom: 8 }}>Unsaved changes</div>
        <div style={{ opacity: 0.8, marginBottom: 16 }}>
          This project has unsaved changes. Save before continuing?
        </div>
        <div style={{ display: "flex", justifyContent: "flex-end", gap: 8 }}>
          {order.map((choice, i) => {
            const isSave = choice === "save";
            const focused = focusIdx === i;
            return (
              <button
                key={choice}
                ref={(el) => { btnRefs.current[i] = el; }}
                onClick={() => onChoose(choice)}
                style={{
                  padding: "4px 12px",
                  ...(isSave
                    ? { fontWeight: 600, background: "#0072B2", color: "#fff", border: "1px solid #0072B2", borderRadius: 4 }
                    : {}),
                  // Offset puts the ring on the white modal background, so it is
                  // visible even on the blue Save button.
                  outline: focused ? "2px solid #0072B2" : "none",
                  outlineOffset: 2,
                }}
              >
                {labels[choice]}
              </button>
            );
          })}
        </div>
      </div>
    </div>
  );
}
