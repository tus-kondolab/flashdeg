# Fetch an Ensembl gene-ID -> gene-symbol map from Ensembl BioMart and write it,
# gzip-compressed, to the GUI's external gene_maps/ folder. These files are NOT
# embedded in the executable; they ship in a `gene_maps/` folder next to the app
# and the GUI reads them at runtime for the gene-ID <-> symbol display toggle.
#
# Usage:   .\scripts\fetch-gene-symbols.ps1                 # human (default)
#          .\scripts\fetch-gene-symbols.ps1 -Species fly    # Drosophila
#          .\scripts\fetch-gene-symbols.ps1 -Species all    # both
# Output:  gui_app/gene_maps/gene_symbols_<species>.tsv.gz  (lines: "ID\tSYMBOL")
#
# Data source: Ensembl BioMart (https://www.ensembl.org), datasets
# hsapiens_gene_ensembl / dmelanogaster_gene_ensembl, attributes
# ensembl_gene_id + external_gene_name. Drosophila gene stable IDs are FlyBase
# FBgn ids. Ensembl data is freely available; see THIRD_PARTY_NOTICES.md.

[CmdletBinding()]
param(
    [ValidateSet("human", "fly", "all")]
    [string]$Species = "human",
    [string]$Mart = "https://www.ensembl.org/biomart/martservice"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $scriptDir "..\gene_maps"

# species -> (BioMart dataset, output file basename)
$datasets = @{
    human = "hsapiens_gene_ensembl"
    fly   = "dmelanogaster_gene_ensembl"
}

function Fetch-Species {
    param([string]$Name)

    $dataset = $datasets[$Name]
    $outGz = Join-Path $outDir "gene_symbols_$Name.tsv.gz"

    # BioMart XML query: every gene's stable ID + display name (no filter).
    $query = @"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE Query>
<Query virtualSchemaName="default" formatter="TSV" header="0" uniqueRows="1" datasetConfigVersion="0.6">
  <Dataset name="$dataset" interface="default">
    <Attribute name="ensembl_gene_id"/>
    <Attribute name="external_gene_name"/>
  </Dataset>
</Query>
"@

    Write-Host "Querying Ensembl BioMart ($dataset)…" -ForegroundColor Cyan
    $resp = Invoke-WebRequest -Uri $Mart -Method Post -Body @{ query = $query } -TimeoutSec 300 -UseBasicParsing
    $raw = $resp.Content

    # Keep only rows that have a non-empty symbol; normalise to "ID\tSYMBOL".
    $pairs = [System.Collections.Generic.List[string]]::new()
    foreach ($line in ($raw -split "`n")) {
        $line = $line.TrimEnd("`r")
        if ($line -eq "") { continue }
        $cols = $line -split "`t"
        if ($cols.Count -lt 2) { continue }
        $id = $cols[0].Trim()
        $sym = $cols[1].Trim()
        if ($id -eq "" -or $sym -eq "") { continue }
        $pairs.Add("$id`t$sym")
    }
    if ($pairs.Count -lt 1000) {
        throw "BioMart returned only $($pairs.Count) rows for $dataset — likely an error page, not the full set."
    }
    $pairs.Sort([System.StringComparer]::Ordinal)
    $text = ($pairs -join "`n") + "`n"

    # gzip-compress to the external gene_maps/ dir (shipped next to the app).
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    $fs = [System.IO.File]::Create($outGz)
    try {
        $gz = New-Object System.IO.Compression.GZipStream($fs, [System.IO.Compression.CompressionLevel]::Optimal)
        try { $gz.Write($bytes, 0, $bytes.Length) } finally { $gz.Dispose() }
    } finally { $fs.Dispose() }

    $gzSize = (Get-Item $outGz).Length
    Write-Host ("Wrote {0}" -f $outGz) -ForegroundColor Green
    Write-Host ("  genes : {0}" -f $pairs.Count)
    Write-Host ("  raw   : {0:N0} bytes" -f $bytes.Length)
    Write-Host ("  gzip  : {0:N0} bytes" -f $gzSize)
    Write-Host ("  fetched: {0:yyyy-MM-dd} from {1}" -f (Get-Date), $Mart)
}

$targets = if ($Species -eq "all") { @("human", "fly") } else { @($Species) }
foreach ($sp in $targets) { Fetch-Species -Name $sp }
