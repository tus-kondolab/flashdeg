// Volcano plot (canvas-based, no Plotly). Thin wrapper over ScatterCanvas using
// the shared row→figure projection (shared with the export preview).

import { memo, useMemo } from "react";
import type { CommonPlotProps } from "./types";
import { buildPanelFigure, panelLabelPoints } from "./panel_figure";
import { ScatterCanvas } from "./ScatterCanvas";

const EMPTY = new Set<string>();

export const Volcano = memo(function Volcano({
  rows,
  padjCutoff,
  log2fcCutoff,
  pub,
  displayName,
  onHover,
  onToggleLabel,
  selectedGene,
  onSelectGene,
  onRendered,
  registerExport,
}: CommonPlotProps) {
  const fig = useMemo(
    () => buildPanelFigure("volcano", rows, padjCutoff, log2fcCutoff),
    [rows, padjCutoff, log2fcCutoff],
  );
  const labelPoints = useMemo(
    () => panelLabelPoints("volcano", rows, pub?.labeledGenes ?? EMPTY, displayName),
    [rows, pub?.labeledGenes, displayName],
  );

  return (
    <ScatterCanvas
      series={fig.series}
      xTitle={fig.xTitle}
      yTitle={fig.yTitle}
      shapes={fig.shapes}
      labelPoints={labelPoints}
      specXRange={pub?.xRange ?? null}
      specYRange={pub?.yRange ?? null}
      revision={pub?.revision ?? 0}
      hoverFormat={fig.hoverFormat}
      onHover={onHover}
      onToggleLabel={onToggleLabel}
      selectedGene={selectedGene}
      onSelectGene={onSelectGene}
      onRendered={onRendered}
      registerExport={registerExport}
    />
  );
});
