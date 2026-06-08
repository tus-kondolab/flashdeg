param(
    [string]$Flashdeg = "",
    [switch]$KeepTemp
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Flashdeg {
    param([string]$Requested)

    if ($Requested) {
        if (-not (Test-Path -LiteralPath $Requested)) {
            throw "flashdeg executable not found: $Requested"
        }
        return (Resolve-Path -LiteralPath $Requested).Path
    }

    $scriptDir = Split-Path -Parent $MyInvocation.ScriptName
    $candidates = @(
        (Join-Path $scriptDir "flashdeg.exe"),
        (Join-Path $scriptDir "flashdeg"),
        (Join-Path $scriptDir "..\flashdeg.exe"),
        (Join-Path $scriptDir "..\flashdeg"),
        (Join-Path $scriptDir "bin\flashdeg.exe"),
        (Join-Path $scriptDir "bin\flashdeg"),
        (Join-Path $scriptDir "..\bin\flashdeg.exe"),
        (Join-Path $scriptDir "..\bin\flashdeg")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $command = Get-Command flashdeg -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "flashdeg executable not found. Pass its path as the first argument."
}

function Require-Column {
    param(
        [string[]]$Columns,
        [string]$Name
    )

    if ($Columns -notcontains $Name) {
        throw "results.csv is missing required column: $Name"
    }
}

function Require-Gene-Result {
    param(
        [hashtable]$RowsByGene,
        [string]$Gene,
        [double]$MinLog2Fc,
        [double]$MaxLog2Fc,
        [double]$MinPValue,
        [double]$MaxPValue,
        [string]$Description
    )

    if (-not $RowsByGene.ContainsKey($Gene)) {
        throw "results.csv is missing expected gene row: $Gene"
    }

    $row = $RowsByGene[$Gene]
    $log2fc = [double]$row.log2FoldChange
    $pvalue = [double]$row.pvalue
    if ($log2fc -lt $MinLog2Fc -or $log2fc -gt $MaxLog2Fc) {
        throw "$Gene log2FoldChange out of expected range for $Description`: $log2fc"
    }
    if ($pvalue -lt $MinPValue -or $pvalue -gt $MaxPValue) {
        throw "$Gene pvalue out of expected range for $Description`: $pvalue"
    }
}

$exe = Resolve-Flashdeg $Flashdeg
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("flashdeg_smoke_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    $counts = Join-Path $tempRoot "counts.csv"
    $metadata = Join-Path $tempRoot "metadata.csv"
    $results = Join-Path $tempRoot "results.csv"

    $countLines = New-Object System.Collections.Generic.List[string]
    $sampleNames = 1..12 | ForEach-Object { "s$_" }
    $countLines.Add("gene_id," + ($sampleNames -join ","))
    for ($gene = 1; $gene -le 10000; $gene++) {
        $base = 20 + (($gene * 37) % 300)
        if ($gene % 4 -eq 0) {
            $effectNumerator = 2
            $effectDenominator = 1
        } elseif ($gene % 5 -eq 0) {
            $effectNumerator = 1
            $effectDenominator = 2
        } else {
            $effectNumerator = 1
            $effectDenominator = 1
        }

        $values = New-Object System.Collections.Generic.List[string]
        for ($sample = 1; $sample -le 12; $sample++) {
            $mean = $base
            if ($sample -gt 6) {
                $mean = [int][math]::Max(
                    1, [math]::Floor(
                        ($base * $effectNumerator + [math]::Floor($effectDenominator / 2)) / $effectDenominator))
            }
            $noise = (($gene * (13 + $sample * 7)) % 41) - 20
            $extra = 0
            if (($gene + $sample) % 11 -eq 0) {
                $extra = [int][math]::Floor(($mean * 45 + 50) / 100)
            }
            $value = [int][math]::Max(1, $mean + $noise + $extra)
            $values.Add([string]$value)
        }
        $countLines.Add(("gene{0:D3}," -f $gene) + ($values -join ","))
    }
    $countLines | Set-Content -LiteralPath $counts -Encoding ASCII

@'
sample_id,condition
s1,control
s2,control
s3,control
s4,control
s5,control
s6,control
s7,treated
s8,treated
s9,treated
s10,treated
s11,treated
s12,treated
'@ | Set-Content -LiteralPath $metadata -Encoding ASCII

    Write-Host "Test: FlashDEG CLI smoke test"
    Write-Host "flashdeg executable: $exe"
    Write-Host "Input: deterministic synthetic 10000 genes x 12 samples"
    Write-Host "Groups: 6 control samples, 6 treated samples"
    Write-Host "Analysis: differential expression for treated vs control using condition as the design factor"
    Write-Host "Model: design '~ condition'"
    Write-Host "Contrast: condition treated vs control"
    Write-Host "Cook handling: default enabled (refit-cooks=true, cooks-filter=true)"
    Write-Host "Independent filtering alpha: 0.05"
    Write-Host "Command:"
    Write-Host "  flashdeg run --counts <counts.csv> --metadata <metadata.csv> --design '~ condition' --ref-level condition=control --contrast condition treated control --alpha 0.05 --threads 1 --quiet --out <results.csv>"
    & $exe --version
    if ($LASTEXITCODE -ne 0) {
        throw "flashdeg --version failed with exit code $LASTEXITCODE"
    }

    & $exe run `
        --counts $counts `
        --metadata $metadata `
        --design "~ condition" `
        --ref-level "condition=control" `
        --contrast "condition" "treated" "control" `
        --alpha 0.05 `
        --threads 1 `
        --quiet `
        --out $results

    if ($LASTEXITCODE -ne 0) {
        throw "flashdeg run failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $results)) {
        throw "results.csv was not created"
    }

    $lines = Get-Content -LiteralPath $results
    if ($lines.Count -lt 2) {
        throw "results.csv has no data rows"
    }

    $columns = $lines[0].TrimEnd("`r").Split(",")
    foreach ($column in @("gene_id", "baseMean", "log2FoldChange", "lfcSE", "stat", "pvalue", "padj")) {
        Require-Column $columns $column
    }

    $rowsByGene = @{}
    foreach ($row in (Import-Csv -LiteralPath $results)) {
        $rowsByGene[$row.gene_id] = $row
    }
    Require-Gene-Result $rowsByGene "gene004" 0.85 1.15 0.0 1e-12 "strong positive treated/control signal"
    Require-Gene-Result $rowsByGene "gene005" -1.35 -0.90 0.0 1e-12 "strong negative treated/control signal"
    Require-Gene-Result $rowsByGene "gene001" -0.20 0.20 0.10 1.0 "near-null signal"

    $deAlpha = 0.05
    $expectedUp = 2475
    $expectedDown = 1479
    $testedPadj = 0
    $upGenes = 0
    $downGenes = 0
    foreach ($row in $rowsByGene.Values) {
        $padj = [double]$row.padj
        if ([double]::IsNaN($padj)) {
            continue
        }
        $testedPadj++
        if ($padj -lt $deAlpha) {
            $log2fc = [double]$row.log2FoldChange
            if ($log2fc -gt 0.0) {
                $upGenes++
            } elseif ($log2fc -lt 0.0) {
                $downGenes++
            }
        }
    }
    if ($upGenes -ne $expectedUp -or $downGenes -ne $expectedDown) {
        throw "DE summary differs from smoke oracle: observed up=$upGenes down=$downGenes; expected up=$expectedUp down=$expectedDown"
    }

    $dataRows = $lines.Count - 1
    Write-Host "Result: results.csv created and validated"
    Write-Host "Output: $results"
    Write-Host "Rows: $dataRows genes"
    Write-Host "DE threshold: padj < $deAlpha"
    Write-Host "DE summary: up=$upGenes genes, down=$downGenes genes"
    Write-Host "Oracle summary: up=$expectedUp genes, down=$expectedDown genes"
    Write-Host "Oracle check: PASS"
    Write-Host "Required columns: gene_id, baseMean, log2FoldChange, lfcSE, stat, pvalue, padj"
    Write-Host "Numerical checks: gene004 positive, gene005 negative, gene001 near-null"
    Write-Host "Status: PASS"
    if ($KeepTemp) {
        Write-Host "Temporary output kept at: $tempRoot"
    } else {
        Write-Host "Temporary files removed."
    }
} finally {
    if (-not $KeepTemp) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
