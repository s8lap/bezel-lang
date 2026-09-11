// fix #4265 (§15.1 "Inline arrays" / §9.7 "Records and disposal" / §28.9
// "Runtime-managed types", v0.298): a `string` local passed as a plain
// (no-modifier) argument to a resolved method whose declared return type is
// an INLINE ARRAY of `string` elements is a heap-use-after-free once that
// method embeds the argument into the returned `[N x elem]` aggregate and
// the argument's own scope ends before the returned alias is read.
//
// ROOT CAUSE: this is the SIXTH channel `Emitter::walkCallArgs`'s #1430
// relaxation misses. `fix #4254` (record_choice_call_result_param_alias_uaf
// _4254_test.cpp, this same directory) found the FIFTH — a callee returning
// a freshly-constructed RECORD or value-like CHOICE embeds the argument via
// `buildRecordValue`'s positional fast path (a bare `CreateStore`, no
// `emitStringRetain`) — and narrowed `calleeReturnMayAliasArgUnretained`
// (LlvmBackend_Setup.cpp) to decline the relaxation when the callee's
// declared return type (`crt`) is `TypeRef::Kind::Named` with a Record/
// Choice declaration. An inline-array return type is
// `TypeRef::Kind::InlineArray`, not `Kind::Named` — the #4254 guard's
// condition can never match it — yet the ReturnStmt inline-array
// materialization block (LlvmBackend_Stmts.cpp, the §15.3 `return [e1, …]`
// branch) stores each element with the identical bare
// `builder_.CreateStore(ev, gep)`, no retain, the same raw-store shape as
// the record/choice fast path. So `Helper.Wrap(s: string): [1]string
// return [s]; end Wrap;` called as `var outer := Helper.Wrap(s);` returns
// an array whose slot 0 ALIASES the caller's `s` with no independent +1,
// while the #1430 relaxation still marks `s` call-bounded, arming `s`'s own
// scope-end release.
//
// §15.1's own "Lifetime of inline arrays carrying borrows" paragraph
// independently names `string` as one of the "runtime-managed
// reference-typed elements" a value-typed inline array holds without
// borrow-tracking ("Inline arrays of value-typed elements, runtime-managed
// reference-typed elements (`string`, function values), ... are not
// borrow-bearing") — confirming nothing else in the compiler stands in for
// the missing retain on this element kind.
//
// FIX: `walkCallArgs` now ALSO declines the relaxation when `crt->kind ==
// sema::TypeRef::Kind::InlineArray` and `crt->arrayElement` is a
// non-nullable-or-nullable `string` primitive — an `else if` arm alongside
// the existing Record/Choice check, sharing the same `crt` (and therefore
// the same local/imported branch #4260 already wired). Same direction as
// #4254: NARROW the relaxation (suppress the caller-side release) rather
// than add a retain to the ReturnStmt inline-array materialization's store
// loop, which is the general local-decl/return inline-array-literal path
// (reached for every element source, not just an aliased call argument) —
// retroactively retaining there would need re-proving against producer-
// owned element shapes this fix has not measured, exactly the risk #4254's
// own rationale declined for the record/choice fast path.
//
// ⚠ CHANNEL COUNT, NOT CLAIMED EXHAUSTIVE: four when #4254 opened, five when
// #4254 itself found the Record/Choice gap, six as of this fix. See the
// commit body for what was checked and what was not (e.g. inline arrays of
// CLASS-typed or CHOICE-typed elements go through a different lowering path
// entirely and are NOT covered by this fix — untested here).
//
// MEASURED (isolated worktree, both legs this tree — `git diff` captured
// before reverting, `git checkout --` to strip the fix, `cmake --build
// build --target bezelc` explicitly per LANE_RULES §23, re-run under
// `scripts/agent_check.sh --probe --asan` / a direct `--sanitize=address`
// standalone compile, `git apply` to restore, rebuild again):
//   - narrow_scope_helper_returned_inline_array_string_param (the issue's
//     own single-element `[1]string` shape): FAIL-WITHOUT is a real
//     `AddressSanitizer: heap-use-after-free` (READ of size 8, freed in
//     `main`, previously allocated in `main`, a 32-byte `bz.string`-header
//     region — the identical signature #4254's own three UAF rows
//     measure). PASS-WITH: clean, exit 5 (`"hello".Length`), no ASan/LSan
//     output at all.
//   - narrow_scope_helper_returned_multi_element_inline_array_string_param
//     (a `[2]string` return, slot 0 the aliased argument, slot 1 an
//     unrelated literal — proves the fix is not a single-slot coincidence):
//     FAIL-WITHOUT is the identical UAF signature. PASS-WITH: clean, exit 5.
// So 2 of 2 UAF rows fail without the fix and pass with it (named above);
// the remaining 3 rows in this file are controls, clean on BOTH sides
// (pinning the axis boundary, not re-measured pre-fix — see each row's own
// comment for why).
//
// A cross-module (IMPORTED) axis was ALSO measured by hand (not committed
// as a gtest row here, to keep this file's scope to the local axis #4265
// itself reports): the identical `[1]string`-returning method exported from
// a separate compiled module and called across the boundary is ALSO a real
// heap-use-after-free pre-fix (same 32-byte signature) and clean of any UAF
// post-fix — but leaks 38 B / 2 allocations (32 direct + 6 indirect)
// post-fix, identically reproducible across repeated runs. This is the
// SAME shape #4260's commit measured and attributed to a pre-existing,
// unrelated gap (issue 4266: "collectRecordCallResultReleaseEligible's
// LocalDecl-only scan never covering an `outer := <call>;` assignment
// target") — not a new defect this fix introduces, and not proven identical
// to 4266 here (a DIFFERENT collector very likely owns inline-array
// call-result release eligibility). Filed as issue 4267 with the measured
// repro rather than guessed at or silently folded into 4266.
//
// See tests/README.md for the `runAot`/`runAotChecked` harness this file
// uses exclusively, and record_choice_call_result_param_alias_uaf_4254_test
// .cpp for the sibling Record/Choice channel this fix sits beside.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <regex>

using bezel::test::runAotChecked;

namespace {
// Strict oracle: compiled, ran, exited cleanly, no ASan/LSan finding. `ran`
// is false on a UAF (which routes through an ASan hard-abort, fork-isolated)
// exactly as it is on a leak — see #4254/#4232's identical pattern.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// leaksAtLeast — a measured, margined leak-magnitude floor oracle (§30: NEVER
// pin an exact byte count; LeakSanitizer's conservative stack scan is
// nondeterministic run to run). Copied from the pattern
// nested_record_string_field_leak_4237_test.cpp established (this tree,
// `leaksAtLeast`'s own comment there records the measured 8248/8290 B
// jitter on an identical binary/program). Used here only for the
// TYPE-KIND-BOUNDARY control (a heap `Array<string>` factory), never for a
// FAIL-WITHOUT/PASS-WITH UAF row.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;  // no sanitizer finding at all — wrongly clean
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;  // sanitizer fired but not with a leak SUMMARY (e.g. a
                    // crash/UAF instead) — correctly fails this row too
}
}  // namespace

// ===========================================================================
// PRIMARY PIN 1/2 — the issue's own repro, verbatim: a single-element
// `[1]string` inline-array return. FAIL-WITHOUT: AddressSanitizer
// heap-use-after-free (measured, see file header). PASS-WITH: exit 5
// ("he"+"llo" = "hello", Length 5).
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_param_alias_uaf_4265,
           narrow_scope_helper_returned_inline_array_string_param_no_longer_uaf) {
    const char* prog =
        "module M; "
        "class Helper "
        "  public static method Wrap(s: string): [1]string "
        "    return [s]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [1]string; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    return outer[0].Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// PRIMARY PIN 2/2 — a TWO-element inline array, where slot 0 is the aliased
// argument and slot 1 is an unrelated literal in the SAME return statement.
// Proves the fix suppresses the relaxation for the whole call (the guard
// keys on the CALLEE's declared return type, not a per-slot analysis), not
// merely for the coincidental single-slot case PIN 1 exercises. FAIL-WITHOUT:
// identical AddressSanitizer heap-use-after-free signature (measured).
// PASS-WITH: exit 5 (slot 0's "hello".Length; slot 1 is never read, so its
// own correctness is not this row's concern).
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_param_alias_uaf_4265,
           narrow_scope_helper_returned_multi_element_inline_array_string_param_no_longer_uaf) {
    const char* prog =
        "module M; "
        "class Helper "
        "  public static method Wrap(s: string): [2]string "
        "    return [s, \"other\"]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [2]string; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    return outer[0].Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// CONTRAST CONTROL — the INLINE form of the identical value, same narrow-
// vs-wide scope shape, never reaches the #1430 relaxation at all (there is
// no CALL — `outer := [s];` builds the array directly, so `ms` is null and
// the walker's whole `stringReleaseMode && ms` gate is moot). Clean before
// and after this fix; included to pin that the fix does not touch the shape
// the issue calls "round-trips correctly", not as a FAIL-WITHOUT row.
// MEASURED clean (exit 5) on the post-fix compiler; not re-measured pre-fix
// since the relaxation this fix touches cannot even be reached here.
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_param_alias_uaf_4265,
           narrow_scope_inline_construction_control_stays_clean) {
    const char* prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [1]string; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := [s]; "
        "    end; "
        "    return outer[0].Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// SCOPE-SHAPE CONTROL — the SAME helper-returned pattern as PIN 1, but in a
// single FLAT scope (no `do ... end;` narrowing `s`). The underlying
// accounting defect is identical pre-fix (`s` is still wrongly classified
// non-escaping), but nothing ever reads the dangling alias before `s`'s own
// scope-end release fires — disposal runs only after the return expression
// is computed, so a flat caller can never OBSERVE this defect. Included to
// document why the issue's own two-line excerpt (no visible block scoping)
// needed a fuller repro to reproduce under ASan, mirroring #4254's own
// identical control. Clean before and after the fix (not re-measured
// pre-fix for the same reason as #4254's twin: this row's whole point is
// that it cannot observe the bug regardless of the guard's state).
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_param_alias_uaf_4265,
           flat_scope_helper_returned_inline_array_stays_clean_and_correct) {
    const char* prog =
        "module M; "
        "class Helper "
        "  public static method Wrap(s: string): [1]string "
        "    return [s]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer := Helper.Wrap(s); "
        "    return outer[0].Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// TYPE-KIND-BOUNDARY CONTROL — a HEAP `Array<string>` factory (going through
// the real `bz_array_alloc`/`bz_array_add` runtime calls, not the inline-
// array `[N x elem]` aggregate this fix touches) is the axis this fix
// deliberately does NOT reach: `stringReleaseMode`'s relaxation for a
// method returning `Array<string>` was never suppressed by #4254 either
// (`crt->kind` is `Named` with a BUILTIN `declaration` that is neither
// Record nor Choice, so neither guard fires), and this fix's InlineArray arm
// cannot match a Named return type at all. The issue's own repro noted this
// shape leaks (not UAFs) because the probe never disposes the returned
// array — an artifact of the probe program, not a defect; loop-amplified
// here (200x, per LANE policy: a one-shot LSan row can pass on both sides)
// to turn that into a measured floor. Not a FAIL-WITHOUT row: this control's
// job is to confirm the fix's boundary, not to re-measure #4254's own scope.
// ===========================================================================
BEZEL_TEST(inline_array_string_call_result_param_alias_uaf_4265,
           heap_array_string_factory_control_still_leaks_not_uaf) {
    const char* prog =
        "module M; "
        "class Helper "
        "  public static method Wrap(s: string): Array<string> "
        "    return [s]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var outer: Array<string>; "
        "      do "
        "        var s := \"he\" + \"llo\" + i.ToString(); "
        "        outer := Helper.Wrap(s); "
        "      end; "
        "      var n := outer[0].Length; "
        "      i += 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(leaksAtLeast(prog, 2000));
}
