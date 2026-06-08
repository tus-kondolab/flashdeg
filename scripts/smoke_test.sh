#!/usr/bin/env sh
set -eu

resolve_flashdeg() {
    requested="${1:-}"
    if [ -n "$requested" ]; then
        if [ ! -f "$requested" ]; then
            echo "error: flashdeg executable not found: $requested" >&2
            exit 1
        fi
        printf '%s\n' "$requested"
        return
    fi

    script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    for candidate in \
        "$script_dir/flashdeg" \
        "$script_dir/flashdeg.exe" \
        "$script_dir/../flashdeg" \
        "$script_dir/../flashdeg.exe" \
        "$script_dir/bin/flashdeg" \
        "$script_dir/bin/flashdeg.exe" \
        "$script_dir/../bin/flashdeg" \
        "$script_dir/../bin/flashdeg.exe"
    do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return
        fi
    done

    if command -v flashdeg >/dev/null 2>&1; then
        command -v flashdeg
        return
    fi

    echo "error: flashdeg executable not found. Pass its path as the first argument." >&2
    exit 1
}

require_column() {
    file="$1"
    column="$2"
    if ! awk -F, -v column="$column" '
        NR == 1 {
            gsub(/\r/, "")
            sub(/^\357\273\277/, "", $1)
            for (i = 1; i <= NF; i++) {
                if ($i == column) {
                    found = 1
                }
            }
            exit(found ? 0 : 1)
        }
    ' "$file"; then
        echo "error: results.csv is missing required column: $column" >&2
        exit 1
    fi
}

require_gene_result() {
    file="$1"
    gene="$2"
    min_lfc="$3"
    max_lfc="$4"
    min_p="$5"
    max_p="$6"
    description="$7"

    if ! awk -F, \
        -v gene="$gene" \
        -v min_lfc="$min_lfc" \
        -v max_lfc="$max_lfc" \
        -v min_p="$min_p" \
        -v max_p="$max_p" \
        -v description="$description" '
        NR == 1 {
            gsub(/\r/, "")
            sub(/^\357\273\277/, "", $1)
            for (i = 1; i <= NF; i++) {
                if ($i == "gene_id") {
                    gene_col = i
                } else if ($i == "log2FoldChange") {
                    lfc_col = i
                } else if ($i == "pvalue") {
                    p_col = i
                }
            }
            next
        }
        {
            gsub(/\r/, "")
            if ($gene_col == gene) {
                found = 1
                lfc = $lfc_col + 0
                pvalue = $p_col + 0
                if (lfc < min_lfc || lfc > max_lfc) {
                    printf "error: %s log2FoldChange out of expected range for %s: %.17g\n", gene, description, lfc > "/dev/stderr"
                    exit 2
                }
                if (pvalue < min_p || pvalue > max_p) {
                    printf "error: %s pvalue out of expected range for %s: %.17g\n", gene, description, pvalue > "/dev/stderr"
                    exit 3
                }
                exit 0
            }
        }
        END {
            if (!found) {
                printf "error: results.csv is missing expected gene row: %s\n", gene > "/dev/stderr"
                exit 4
            }
        }
    ' "$file"; then
        exit 1
    fi
}

native_path() {
    case "$flashdeg" in
        *.exe|*\\*)
            if command -v cygpath >/dev/null 2>&1; then
                cygpath -w "$1"
                return
            elif command -v wslpath >/dev/null 2>&1; then
                wslpath -w "$1"
                return
            fi
            ;;
    esac
    printf '%s\n' "$1"
}

flashdeg=$(resolve_flashdeg "${1:-}")
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/flashdeg_smoke.XXXXXX")
keep_temp="${FLASHDEG_SMOKE_KEEP_TEMP:-0}"

cleanup() {
    if [ "$keep_temp" != "1" ]; then
        rm -rf "$tmp_dir"
    fi
}
trap cleanup EXIT INT TERM

counts="$tmp_dir/counts.csv"
metadata="$tmp_dir/metadata.csv"
results="$tmp_dir/results.csv"
counts_arg=$(native_path "$counts")
metadata_arg=$(native_path "$metadata")
results_arg=$(native_path "$results")

{
    printf 'gene_id'
    sample=1
    while [ "$sample" -le 12 ]; do
        printf ',s%s' "$sample"
        sample=$((sample + 1))
    done
    printf '\n'

    gene=1
    while [ "$gene" -le 10000 ]; do
        base=$((20 + (gene * 37) % 300))
        if [ $((gene % 4)) -eq 0 ]; then
            effect_num=2
            effect_den=1
        elif [ $((gene % 5)) -eq 0 ]; then
            effect_num=1
            effect_den=2
        else
            effect_num=1
            effect_den=1
        fi

        printf 'gene%03d' "$gene"
        sample=1
        while [ "$sample" -le 12 ]; do
            mean=$base
            if [ "$sample" -gt 6 ]; then
                mean=$(((base * effect_num + effect_den / 2) / effect_den))
                if [ "$mean" -lt 1 ]; then
                    mean=1
                fi
            fi
            noise=$(((gene * (13 + sample * 7)) % 41 - 20))
            extra=0
            if [ $(((gene + sample) % 11)) -eq 0 ]; then
                extra=$(((mean * 45 + 50) / 100))
            fi
            value=$((mean + noise + extra))
            if [ "$value" -lt 1 ]; then
                value=1
            fi
            printf ',%s' "$value"
            sample=$((sample + 1))
        done
        printf '\n'
        gene=$((gene + 1))
    done
} > "$counts"

cat > "$metadata" <<'CSV'
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
CSV

echo "Test: FlashDEG CLI smoke test"
echo "flashdeg executable: $flashdeg"
echo "Input: deterministic synthetic 10000 genes x 12 samples"
echo "Groups: 6 control samples, 6 treated samples"
echo "Analysis: differential expression for treated vs control using condition as the design factor"
echo "Model: design '~ condition'"
echo "Contrast: condition treated vs control"
echo "Cook handling: default enabled (refit-cooks=true, cooks-filter=true)"
echo "Independent filtering alpha: 0.05"
echo "Command:"
echo "  flashdeg run --counts <counts.csv> --metadata <metadata.csv> --design '~ condition' --ref-level condition=control --contrast condition treated control --alpha 0.05 --threads 1 --quiet --out <results.csv>"
"$flashdeg" --version

"$flashdeg" run \
    --counts "$counts_arg" \
    --metadata "$metadata_arg" \
    --design "~ condition" \
    --ref-level "condition=control" \
    --contrast "condition" "treated" "control" \
    --alpha 0.05 \
    --threads 1 \
    --quiet \
    --out "$results_arg"

if [ ! -f "$results" ]; then
    echo "error: results.csv was not created" >&2
    exit 1
fi

line_count=$(wc -l < "$results" | tr -d ' ')
if [ "$line_count" -lt 2 ]; then
    echo "error: results.csv has no data rows" >&2
    exit 1
fi

for column in gene_id baseMean log2FoldChange lfcSE stat pvalue padj; do
    require_column "$results" "$column"
done

require_gene_result "$results" "gene004" 0.85 1.15 0.0 1e-12 "strong positive treated/control signal"
require_gene_result "$results" "gene005" -1.35 -0.90 0.0 1e-12 "strong negative treated/control signal"
require_gene_result "$results" "gene001" -0.20 0.20 0.10 1.0 "near-null signal"

de_alpha="0.05"
expected_up=2475
expected_down=1479
summary=$(awk -F, -v de_alpha="$de_alpha" '
    NR == 1 {
        gsub(/\r/, "")
        sub(/^\357\273\277/, "", $1)
        for (i = 1; i <= NF; i++) {
            if ($i == "log2FoldChange") {
                lfc_col = i
            } else if ($i == "padj") {
                padj_col = i
            }
        }
        next
    }
    {
        gsub(/\r/, "")
        padj_text = $padj_col
        if (padj_text == "" || padj_text == "nan" || padj_text == "NaN") {
            next
        }
        padj = padj_text + 0
        tested++
        if (padj < de_alpha) {
            lfc = $lfc_col + 0
            if (lfc > 0) {
                up++
            } else if (lfc < 0) {
                down++
            }
        }
    }
    END {
        printf "%d %d %d\n", tested, up, down
    }
' "$results")
set -- $summary
tested_padj="$1"
up_genes="$2"
down_genes="$3"
if [ "$up_genes" -ne "$expected_up" ] || [ "$down_genes" -ne "$expected_down" ]; then
    echo "error: DE summary differs from smoke oracle: observed up=$up_genes down=$down_genes; expected up=$expected_up down=$expected_down" >&2
    exit 1
fi

data_rows=$((line_count - 1))
echo "Result: results.csv created and validated"
echo "Output: $results"
echo "Rows: $data_rows genes"
echo "DE threshold: padj < $de_alpha"
echo "DE summary: up=$up_genes genes, down=$down_genes genes"
echo "Oracle summary: up=$expected_up genes, down=$expected_down genes"
echo "Oracle check: PASS"
echo "Required columns: gene_id, baseMean, log2FoldChange, lfcSE, stat, pvalue, padj"
echo "Numerical checks: gene004 positive, gene005 negative, gene001 near-null"
echo "Status: PASS"
if [ "$keep_temp" = "1" ]; then
    echo "Temporary output kept at: $tmp_dir"
else
    echo "Temporary files removed."
fi
