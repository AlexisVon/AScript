#!/bin/bash
# AScript — command-line coverage test suite
# Runs every .axc under cover/ through the AScript binary.
# Silent on pass, only outputs failures.

SCRIPT_DIR=$(dirname "$(realpath "$0")")
ALEXIS="$SCRIPT_DIR/../../bin/axc-cli"
COVER_DIR="$SCRIPT_DIR/cover"
LINK_DIR="$(realpath "$SCRIPT_DIR/link")"

# ── Build test link modules ──
echo "=== Building test link modules ==="
cd "$SCRIPT_DIR"
make -j7
echo ""

# ── Expected-to-fail tests (non-zero exit = pass) ──
XFAIL=(
    "exceptions/31_parse_error.axc"       # intentional syntax error
    "exceptions/04_uncaught.axc"           # uncaught exception prints call stack
    "exceptions/26_stack_error.axc"        # deep recursion blows the stack
    "exceptions/32_lex_empty_prefix.axc"   # 0x/0o/0b without digits
    "exceptions/33_lex_newline_in_string.axc" # newline in string literal
    "exceptions/34_bridge_not_at_end.axc"  # bridge without nav prefix
    "exceptions/36_float_overflow_inf.axc" # float() overflow -> ConvError
    "exceptions/37_triple_dot.axc"         # odd number of dots
    "exceptions/38_invalid_escape.axc"     # invalid escape sequence
    "exceptions/39_nesting_too_deep.axc"   # deep unary chain -> nesting guard
    "exceptions/40_bridge_no_prefix.axc"   # bridge without nav prefix
    "excall/error.axc"                     # undefined extension function
    "functions/test_name_collision.axc"    # def after var -> NameError
    "functions/test_tco_mutual.axc"        # mutual recursion not TCO -> overflow
    "functions/test_tco_nontail.axc"       # non-tail recursion -> overflow
    "modules/t1b_alias_conflict.axc"       # import alias conflict is fatal
)

# ── Files needing special flags ──
declare -A FLAGS
FLAGS["exceptions/18_overflow_error.axc"]="--overflow-check"
FLAGS["exceptions/19_shift_error.axc"]="--overflow-check"
FLAGS["exceptions/17_div_overflow_error.axc"]="--overflow-check"
FLAGS["exceptions/26_stack_error.axc"]="--max-stack 16"
FLAGS["exceptions/27_resource_error.axc"]="--max-vecfill 3"

# Import tests: run from cover/ so "modules/xxx.axc" resolves
declare -A CWD
CWD["modules/t1_import.axc"]="$COVER_DIR"
CWD["modules/t1b_alias_conflict.axc"]="$COVER_DIR"
CWD["modules/t7b_parent_nav.axc"]="$COVER_DIR"
CWD["modules/t4_nav.axc"]="$COVER_DIR"
CWD["modules/t2_link.axc"]="$COVER_DIR"
CWD["modules/t5_delete.axc"]="$COVER_DIR"
CWD["modules/t7_constraints.axc"]="$COVER_DIR"
CWD["modules/t8_link_area.axc"]="$COVER_DIR"
CWD["scope/03i_link_var.axc"]="$COVER_DIR"
CWD["control/args_eval.axc"]="$COVER_DIR"

is_xfail() {
    for xf in "${XFAIL[@]}"; do
        [[ "$1" == "$xf" ]] && return 0
    done
    return 1
}

# ── Run all .axc files ──
failed_count=0
total_count=0

while IFS= read -r -d '' f; do
    rel="${f#"$COVER_DIR/"}"
    total_count=$((total_count + 1))

    flag="${FLAGS[$rel]}"
    run_dir="${CWD[$rel]:-$COVER_DIR}"

    if is_xfail "$rel"; then
        # Expected to fail: non-zero exit = pass
        output=$(cd "$run_dir" && "$ALEXIS" -p "$LINK_DIR" $flag "$COVER_DIR/$rel" 2>&1)
        ec=$?
        if [ $ec -eq 0 ]; then
            echo "=== XPASS (expected fail but passed): $rel ==="
            failed_count=$((failed_count + 1))
        fi
    else
        # Expected to pass: zero exit = pass
        output=$(cd "$run_dir" && "$ALEXIS" -p "$LINK_DIR" $flag "$COVER_DIR/$rel" 2>&1)
        ec=$?
        if [ $ec -ne 0 ]; then
            echo "=== FAIL (exit=$ec): $rel ==="
            echo "$output"
            echo ""
            failed_count=$((failed_count + 1))
        fi
    fi
done < <(find "$COVER_DIR" -name "*.axc" ! -path "$COVER_DIR/modules/modules/fnsimple.axc" -print0 | sort -z)

# ══════════════════════════════════════════════════════════════════════════════
# CLI smoke tests — process-level behavior (args / modes / exit codes / output)
# ══════════════════════════════════════════════════════════════════════════════
echo "=== CLI smoke tests ==="
CLI_FAIL=0
CLI_TOTAL=0
CLI_TMP=$(mktemp -d)
trap 'rm -rf "$CLI_TMP"' EXIT

printf 'var x = ;\n' > "$CLI_TMP/bad.axc"
printf 'var x = 1;\n$io_print(x);\n' > "$CLI_TMP/ok.axc"
printf 'var x = 1 / 0;\n' > "$CLI_TMP/divzero.axc"

# expect_cli <exit_code> <desc> -- <cmd...>
expect_cli() {
    local want=$1; shift
    local desc=$1; shift
    shift
    CLI_TOTAL=$((CLI_TOTAL + 1))
    "$@" >/dev/null 2>&1
    local ec=$?
    if [ $ec -ne "$want" ]; then
        echo "=== CLI FAIL: $desc (exit=$ec, want=$want) ==="
        CLI_FAIL=$((CLI_FAIL + 1))
    fi
}

# expect_out <pattern> <desc> -- <cmd...> — exit 0 and combined output matches
expect_out() {
    local pat=$1; shift
    local desc=$1; shift
    shift
    CLI_TOTAL=$((CLI_TOTAL + 1))
    local out
    out=$("$@" 2>&1)
    local ec=$?
    if [ $ec -ne 0 ] || ! echo "$out" | grep -q "$pat"; then
        echo "=== CLI FAIL: $desc (exit=$ec) ==="
        echo "$out" | head -3
        CLI_FAIL=$((CLI_FAIL + 1))
    fi
}

# expect_err <pattern> <desc> -- <cmd...> — non-zero exit and output matches
expect_err() {
    local pat=$1; shift
    local desc=$1; shift
    shift
    CLI_TOTAL=$((CLI_TOTAL + 1))
    local out
    out=$("$@" 2>&1)
    local ec=$?
    if [ $ec -eq 0 ] || ! echo "$out" | grep -q "$pat"; then
        echo "=== CLI FAIL: $desc (exit=$ec) ==="
        echo "$out" | head -3
        CLI_FAIL=$((CLI_FAIL + 1))
    fi
}

# usage errors (exit 2)
expect_cli 2 "unknown option" -- "$ALEXIS" --bogus
expect_cli 2 "-p missing arg" -- "$ALEXIS" -p
expect_cli 2 "--max-stack bad number" -- "$ALEXIS" --max-stack abc
expect_cli 2 "conflicting modes" -- "$ALEXIS" -c -x "$CLI_TMP/ok.axc"
expect_cli 2 "multiple files" -- "$ALEXIS" "$CLI_TMP/ok.axc" "$CLI_TMP/divzero.axc"
expect_cli 2 "both -e and file" -- "$ALEXIS" -e 'x;' "$CLI_TMP/ok.axc"
expect_cli 2 "-o without -c" -- "$ALEXIS" -o out.axp "$CLI_TMP/ok.axc"
expect_cli 2 "--embed without -c" -- "$ALEXIS" --embed "$CLI_TMP/ok.axc"
expect_out '{}' "--doc invalid regex -> empty" -- "$ALEXIS" --doc '['

# script error reporting (exit code + message)
expect_err "Unexpected token" "parse error" -- "$ALEXIS" "$CLI_TMP/bad.axc"
expect_err "DivZeroError" "runtime error" -- "$ALEXIS" "$CLI_TMP/divzero.axc"
# handle-typed extensions: a non-handle argument must raise TypeError, never crash
expect_err "TypeError" "io_write non-handle arg" -- "$ALEXIS" -e '$io_write("x");'
expect_err "TypeError" "io_read non-handle arg" -- "$ALEXIS" -e '$io_read("x");'
expect_err "TypeError" "thd_join non-handle arg" -- "$ALEXIS" -e '$thd_join("x");'
expect_err "TypeError" "thd_running non-handle arg" -- "$ALEXIS" -e '$thd_running(1);'

# modes
expect_cli 0 "-e inline exec" -- "$ALEXIS" -e 'var x = 1;'
expect_cli 0 "-c compile" -- "$ALEXIS" -c "$CLI_TMP/ok.axc"
expect_cli 0 "-x exec compiled" -- "$ALEXIS" -x "$CLI_TMP/ok.axp"
expect_cli 0 "-c -e inline compile" -- bash -c "cd '$CLI_TMP' && '$ALEXIS' -c -e 'var y = 1;'"
expect_cli 0 "-c --embed compile + -x exec" -- bash -c "cd '$CLI_TMP' && '$ALEXIS' -c --embed ok.axc && '$ALEXIS' -x ok.axp"

# output checks
expect_out "PROGRAM" "-j AST dump" -- "$ALEXIS" -j "$CLI_TMP/ok.axc"
expect_out 'io_print' "--doc full" -- "$ALEXIS" --doc
expect_out 'math_clamp' "--doc regex filter" -- "$ALEXIS" --doc '^math_'
expect_out '"defines"' "--doc includes defines" -- "$ALEXIS" --doc 'math_PI'
expect_out '\[ret\] 31' "--ret prints the return value" -- "$ALEXIS" --ret -e 'return 31;'
expect_out '\[ret\] \[9, 8, 7\]' "--ret prints containers" -- "$ALEXIS" --ret -e 'return [9, 8, 7];'
expect_out 'io_print' "--toc full" -- "$ALEXIS" --toc
expect_out 'math_clamp' "--toc regex filter" -- "$ALEXIS" --toc '^math_'
# --toc is names only: no manual fields, no JSON punctuation
CLI_TOTAL=$((CLI_TOTAL + 1))
if "$ALEXIS" --toc | grep -qE '"desc"|[{}:]'; then
    echo "=== CLI FAIL: --toc leaked manual content ==="
    "$ALEXIS" --toc | grep -nE '"desc"|[{}:]' | head -3
    CLI_FAIL=$((CLI_FAIL + 1))
fi
# every --toc name must exist in --doc (same source, so only the renderer can drop one)
CLI_TOTAL=$((CLI_TOTAL + 1))
toc_doc=$("$ALEXIS" --doc)
toc_missing=""
while IFS= read -r toc_name; do
    case "$toc_doc" in *"\"$toc_name\""*) ;; *) toc_missing="$toc_missing $toc_name" ;; esac
done <<< "$("$ALEXIS" --toc | tr -d '$')"
if [ -n "$toc_missing" ]; then
    echo "=== CLI FAIL: --toc names missing from --doc:$toc_missing ==="
    CLI_FAIL=$((CLI_FAIL + 1))
fi
expect_out "3" "-i REPL" -- bash -c "printf '1+2;\nexit\n' | '$ALEXIS' -i"
expect_out "axc-1.0.0" "-v version" -- "$ALEXIS" -v

# ══════════════════════════════════════════════════════════════════════════════
# .ax project smoke tests — config-driven run / compile / flag behavior
# ══════════════════════════════════════════════════════════════════════════════
echo "=== .ax project smoke tests ==="
AX_DIR="$SCRIPT_DIR/ax"
AX_PROJ="$AX_DIR/proj"

# .ax run (config-driven execution)
expect_out "hello" ".ax run" -- "$ALEXIS" "$AX_PROJ/run.ax"

# .ax compile (normal)
expect_cli 0 ".ax compile" -- bash -c "'$ALEXIS' -c '$AX_PROJ/run.ax' && test -f '$AX_PROJ/run.axp'"
rm -f "$AX_PROJ/run.axp"

# .ax "out" dir: default target goes there and the directory is created
AX_OUT_DIR="$(mktemp -d)"
printf '{"main": "main.axc", "out": "%s"}\n' "$AX_OUT_DIR" > "$AX_PROJ/outdir.ax"
expect_cli 0 ".ax out dir" -- bash -c "'$ALEXIS' -c '$AX_PROJ/outdir.ax' && test -f '$AX_OUT_DIR/outdir.axp'"
rm -f "$AX_PROJ/outdir.ax"
rm -rf "$AX_OUT_DIR"

# plain .axc compile creates nested output directories
AX_NEST="$(mktemp -d)"
printf '$io_print("nested\\n");\n' > "$AX_NEST/n.axc"
expect_cli 0 ".axc nested out" -- bash -c "'$ALEXIS' -c '$AX_NEST/n.axc' -o '$AX_NEST/a/b/n.axp' && test -f '$AX_NEST/a/b/n.axp'"
rm -rf "$AX_NEST"

# .ax compile embed
expect_cli 0 ".ax compile embed" -- bash -c "'$ALEXIS' -c --embed '$AX_PROJ/run.ax' && test -f '$AX_PROJ/run.axp'"
# keep run.axp for -x test below

# .ax -x exec (run the compiled .axp from the embed step above)
expect_out "hello" ".ax -x exec" -- "$ALEXIS" -x "$AX_PROJ/run.axp"
rm -f "$AX_PROJ/run.axp"

# .ax compile with hint
expect_cli 0 ".ax hint compile" -- bash -c "'$ALEXIS' -c '$AX_PROJ/hint.ax' && test -f '$AX_PROJ/hint.axp'"
rm -f "$AX_PROJ/hint.axp"

# .ax flag rejection: -j and -f are not supported for .ax
expect_err "support run and -c only" ".ax reject -j" -- "$ALEXIS" -j "$AX_PROJ/run.ax"
expect_err "support run and -c only" ".ax reject -f" -- "$ALEXIS" -f "$AX_PROJ/run.ax"

# .ax bad config (missing main)
expect_err "missing" ".ax bad config" -- "$ALEXIS" "$AX_DIR/bad.ax"

echo "=== Summary ==="
echo "Cover: Total: $total_count, Failed: $failed_count"
echo "CLI:   Total: $CLI_TOTAL, Failed: $CLI_FAIL"
if [ $failed_count -gt 0 ] || [ $CLI_FAIL -gt 0 ]; then
    exit 1
fi
