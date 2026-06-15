// Okabe-Ito color-blind safe palette. See gui_plan.md § 15 PoC checklist.

export const VOLCANO_COLORS = {
  ns: "rgba(128, 128, 128, 0.35)",
  down: "rgba(0, 114, 178, 0.75)",
  up: "rgba(213, 94, 0, 0.75)",
  selectedFill: "rgba(240, 228, 66, 1.0)",
  selectedStroke: "rgba(0, 0, 0, 1.0)",
} as const;
