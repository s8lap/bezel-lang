// End-to-end VALUE test for the #2597 fix (const-local narrowing preserved
// across a lambda boundary, §7.4 v0.236).
//
// The sema-level tests for #2597 live in
// `tests/sema/const_local_narrowing_lambda_2597_test.cpp` and assert only that
// the program type-checks. This file asserts the thing that actually matters:
// that the value observed THROUGH the §27.5 capture cell is the right one.
//
// Why this file exists. #2597 lifts a rejection, which routes a previously
// unreachable value shape into codegen: a nullable local, proven non-null by
// §7.4 narrowing, read inside a lambda body through a by-reference capture
// cell. §7.4 narrowing deliberately does NOT rewrite the variable's type — the
// declared nullable type is preserved so codegen's tagged `{hasValue; payload}`
// layout stays correct (`nullNarrowed_`'s declaration comment in
// ExprTypecheck_impl.hpp) — so the unwrap happens at the use site. If a capture
// cell round-trip lost or mis-shaped that layout, the failure mode would be a
// SILENT WRONG VALUE (a 0 where the payload should be), not a crash. A
// type-check-only test cannot see that; only running the program can.
//
// Result: the captured const-narrowed nullable reads back correctly.
//
// NOTE on a related defect found while writing this test. The originally
// proposed shape used `new Atomic<int>(<const-narrowed nullable>)` inside the
// lambda body. That returns 0 instead of the payload — but the cause is NOT
// this fix and NOT the capture cell: `new Atomic<T>` / `new AtomicInteger<T>`
// mis-handle a narrowed-nullable initializer with no lambda anywhere in the
// program, while an ordinary constructor and an ordinary method call in the
// same narrowing context both get it right. That is filed separately as a HIGH
// silent-wrong-value bug; it is deliberately NOT asserted here, because a test
// pinning a known-wrong value would be a red test, and pinning the correct one
// would be a red test too. The control below — a plain read of the same
// captured const-narrowed nullable — is what isolates this fix's own
// contribution, and it is correct.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// -999 distinguishes "did not run / faulted" from any legitimate exit code, so
// a compile reject can never be mistaken for a wrong value.

// Memory-checked oracle (ASan/LSan): used where the shape is leak-clean.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// VALUE-only oracle. Used for the two shapes that currently leak the 8-byte
// closure capture cell (issue #2640 — a codegen defect this fix makes
// REACHABLE, not one it introduces: the value is correct, the cell is not
// released when a const local's fact rides in and the body reads it without an
// in-body null test). Asserting the value here pins exactly what #2597 is
// responsible for, without encoding the leak as expected behavior. Switch
// these two to `runChecked` when #2640 lands.
int runValue(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §7.4 / §27.5 (#2597): a `const` local of nullable type, narrowed non-null
// before the lambda and read inside the body through the capture cell, yields
// the PAYLOAD (42), not the nullable's zero/tag. Rejected outright before the
// fix, so this shape could not previously reach codegen at all.
BEZEL_TEST(codegen_const_narrowing_capture_2597, captured_const_narrowed_int_reads_payload_spec_7_4) {
    int got = runValue(   // #2640: leaks the capture cell; value is correct
        "module M; "
        "class C "
        "  public static method Run(v: int?): int "
        "    const t: int? := v; "
        "    if t <> null then "
        "      var f := lambda(): int => do return t; end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(42); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(got == 42);
}

// §7.4 / §27.5 (#2597): the reference-type form — the issue's own repro shape —
// observed as a VALUE. `"abcd".Length` is 4, so a 0 here would mean the capture
// cell handed the body a null receiver (the #2259 failure mode) rather than the
// narrowed string.
BEZEL_TEST(codegen_const_narrowing_capture_2597, captured_const_narrowed_string_reads_length_spec_7_4) {
    int got = runChecked(
        "module M; "
        "class C "
        "  public static method Run(s: string?): int "
        "    if s = null then return 0; end; "
        "    const t: string? := s; "
        "    if t <> null then "
        "      var f := lambda(): int => do return t.Length; end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(\"abcd\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(got == 4);
}

// §7.4 (#2597): the readonly-field-off-a-const-root form, observed as a value.
// The fact rides its root; the root is `const` and the field is `readonly`, so
// the whole path is final. 4 = "abcd".Length read through `t._f` inside the
// body.
BEZEL_TEST(codegen_const_narrowing_capture_2597, captured_const_rooted_readonly_field_reads_value_spec_7_4) {
    int got = runValue(   // #2640: leaks the capture cell; value is correct
        "module M; "
        "class Holder "
        "  public readonly _f: string?; "
        "  public constructor(v: string?) "
        "    _f := v; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Run(h: Holder): int "
        "    const t: Holder := h; "
        "    if t._f <> null then "
        "      var g := lambda(): int => do return t._f.Length; end; "
        "      return g(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var h := new Holder(\"abcd\"); "
        "    return Run(h); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(got == 4);
}

// §7.4 / §27.5 (#2597): the loop-capture shape — the one most likely to break a
// "const facts ride in" rule. The lambda is created in iteration 0 over a
// `const t` bound to "abcd" and stored in a local that OUTLIVES the loop;
// iteration 1 then executes `const t := null` (a fresh binding). If capture
// were by a shared per-loop cell, the stored closure would read null and fault
// or return 0. It returns 4 — capture is per-iteration, so a later iteration's
// fresh const binding cannot reach the earlier closure's cell.
// #4620 (2026-09-22): moved from `runChecked` to `runValue`, joining the two
// rows above. This shape leaks the SAME #2640 capture cell they do; its
// `runChecked` green was the one-shot-LSan accident CLAUDE.md warns about —
// LSan reports only UNREACHABLE allocations, and at a single iteration the
// cell is still reachable from a live stack slot, so the row passed while
// leaking. #4593 (host DataLayout) changed the stack alignment and removed
// the accident; it did NOT introduce a leak. MEASURED, same program amplified
// to 500 iterations under generated-code ASan:
//     origin/main cafdfc52 : 31,936 B leaked in 1,497 allocations
//     this batch's HEAD    : 31,992 B leaked in 1,499 allocations
// i.e. ~64 B/iteration on BOTH sides. The VALUE assertion below is unchanged
// and still the point of the row (per-iteration capture returns 4, not 0/null).
// Switch back to `runChecked` when #2640 lands, together with the two rows
// above.
BEZEL_TEST(codegen_const_narrowing_capture_2597, loop_declared_const_captures_per_iteration_spec_27_5) {
    int got = runValue(   // #2640/#4620: leaks the capture cell; value is correct
        "module M; "
        "class C "
        "  public static method Pick(i: int): string? "
        "    if i = 0 then return \"abcd\"; end; "
        "    return null; "
        "  end Pick; "
        "  public static method Main(): int "
        "    var f: (() -> int)? := null; "
        "    var i := 0; "
        "    while i < 2 do "
        "      const t: string? := Pick(i); "
        "      if t <> null then "
        "        f := lambda(): int => do return t.Length; end; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    if f <> null then "
        "      return f!(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(got == 4);
}
