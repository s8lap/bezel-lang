// Codegen / refcounting — WRITING to a variable a lambda has already CAPTURED
// double-freed the value written. Issue #2706.
//
// Spec: §27.5 "Closures" — "Captured variables are heap-allocated in a closure
// object. Modifications to a captured variable inside the lambda are visible
// outside, and vice versa." (The heap cell is SHARED by the enclosing frame and
// every closure env; that sharing is the implementation of this rule.); §28.9
// "Runtime-managed types" (`string` and function values are refcounted: every
// co-owner takes exactly one reference and releases exactly one); §28.1 "Where
// the `owned` modifier can appear" — "On any other type — a primitive, `string`,
// a function type, an ordinary record, an enum, a non-owning inline array, or
// the unit type — `owned` is a compile error: such values carry no disposal
// obligation to transfer (§28.9)". Spec revision v0.240.
//
// THE BUG. The per-capture retain (LlvmBackend_Exprs2.cpp, the captured-PARAMS
// loop from #2689 and the captured-LOCALS `alreadyBoxed` loop from #672) loads
// the cell and retains ITS CONTENTS AT CAPTURE TIME. The generated
// `bz$lam$N$envdtor` loads the same cell and releases ITS CONTENTS AT
// DESTRUCTION TIME. A write to the variable between those two points replaces
// the contents, so the retains and the releases land on DIFFERENT strings: the
// displaced value keeps a reference nobody will drop (a leak), and the new value
// takes one release per env against its single +1 —
//
//     AddressSanitizer: SEGV on unknown address 0x000000000009
//       #2 bz_string_release  #3 bz$lam$0$envdtor  #4 bz_closure_release
//
// It is NOT the parameter defect #2696 corrected: a captured LOCAL reproduces it
// with no parameter in the program at all (case A), which is why the fix is in
// the shared-cell machinery and not in either storage class.
//
// THE FIX, AND WHY IT LEAKS ON PURPOSE. Keying the envdtor's release to the
// value that was retained is not possible without refcounting the CELL: the
// closures read THROUGH the cell, so the value the LAST write leaves there must
// stay alive until the LAST env dies, and that env count is dynamic (one lambda
// node inside a loop makes an unbounded number of envs over one guarded cell —
// case D). So for a cell that is both WRITTEN and shared by more than one env,
// the per-capture retain and the envdtor release are suppressed TOGETHER,
// leaving the cell's final value leaked rather than over-released. A bounded
// leak in place of a use-after-free is the prime-directive trade.
//
// ⚠ WHY EVERY STRING IS BUILT AT RUNTIME. `"ab" + "cd"` constant-folds to an
// RC-SENTINEL on which `bz_string_release` is a NO-OP, so a literal-built probe
// passes whether or not the bug is present. Every program below builds its
// strings with interpolation (`$"v-{i}"`).
//
// ⚠ WHY A/B/D/G RUN ON THE PLAIN `runAot` PATH. Post-fix these programs leak the
// cell's final value BY DESIGN (see above), so LSan would fail them for the
// trade the fix deliberately makes. The pre-fix defect is a DOUBLE FREE, which
// aborts the process, so `r.ran` plus an exit code that reads every string back
// THROUGH the closures is a live oracle on the plain path — verified: pre-fix
// these abort with `free(): invalid pointer` / SIGSEGV unsanitized, not only
// under ASan. C, E and F end on a literal or never write, so they are leak-free
// and use `runAotChecked` with LSan as an oracle in both directions.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// =====================================================================
// (A) The issue's repro B: a captured LOCAL, written after TWO captures. NO
// parameter is captured, which is what proves this is the shared cell's defect
// and not #2696's parameter one. Pre-fix: `free(): invalid pointer` (plain) /
// `SEGV in bz$lam$0$envdtor` (ASan).
// Per call, p = "v-1".."v-9" (length 3): s is length 4 after `p + "!"`, then
// length 6 after `s + "-x"`. Both closures read the CELL, so both see the
// post-write value: 6 + 6 + 6 = 18. Nine calls: 162.
// =====================================================================
const char* kWriteAfterTwoCapturesOfLocal =
    "module M; class C "
    "  public static method Two(p: string): int "
    "    var s := p + \"!\"; "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    s := s + \"-x\"; "
    "    return f1(1).Length + f2(2).Length + s.Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..9 do "
    "      total := total + C.Two($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                            // 162
    "  end Main; "
    "end C;";

// =====================================================================
// (B) The issue's repro A: the same write, but the captured variable is a
// `string` PARAMETER. #2696 fixed the unpaired drop-on-overwrite release of the
// caller's borrow here and explicitly recorded THIS as still broken; the two
// fixes compose (the parameter's slot owns its own +1, and the write now
// displaces that one instead of the caller's).
// Per call: s is length 5 after `s + "-x"`; 5 + 5 + 5 = 15. Nine calls: 135.
// =====================================================================
const char* kWriteAfterTwoCapturesOfParam =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    s := s + \"-x\"; "
    "    return f1(1).Length + f2(2).Length + s.Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..9 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 135
    "  end Main; "
    "end C;";

// =====================================================================
// (C) THE LEAK-CHECKED DIRECTION, and the guard against "fixing" this by only
// deleting the envdtor release. The write's RHS is a literal, so the cell's
// final value is an RC-sentinel that leaks nothing — which makes LSan a live
// oracle for the OTHER half. Pre-fix the second capture's `str.recap` retain
// raised the displaced runtime string to rc 2 while the write released only one,
// stranding it: nine leaked strings, `r.ran` false. Post-fix that retain is not
// emitted, the write's release drops the producer's sole +1, and the run is
// clean. Had the fix removed the envdtor release but KEPT the capture retain,
// this case would still leak and stay red.
// Per call: "lit" is length 3, and both closures read the cell: 3 + 3 + 3 = 9.
// Nine calls: 81.
// =====================================================================
const char* kWriteLiteralAfterTwoCaptures =
    "module M; class C "
    "  public static method Two(p: string): int "
    "    var s := p + \"!\"; "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    s := \"lit\"; "
    "    return f1(1).Length + f2(2).Length + s.Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..9 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 81
    "  end Main; "
    "end C;";

// =====================================================================
// (D) ONE lambda NODE, inside a LOOP — the case that rules out counting capture
// sites statically. `s` is declared outside the loop, so boxLocalSlot uses
// GUARDED boxing: ONE cell is allocated on the first pass and reused by every
// iteration's closure. Five envs therefore share one cell from a single
// syntactic capture, and the number of envs is a runtime quantity. Pre-fix this
// reported `heap-use-after-free` — the first envdtor freed the string the next
// iteration then read back through the cell.
// Per outer call (p length 3): s runs 4,6,8,10,12 as the closures read it and
// 14 at the end, so acc = 4+6+8+10+12 = 40 and the result is 54. Four calls: 216.
// =====================================================================
const char* kWriteInLoopWithCaptureInLoop =
    "module M; class C "
    "  public static method Loop(p: string): int "
    "    var s := p + \"!\"; "
    "    var acc := 0; "
    "    for var i in 1..5 do "
    "      var f1 := lambda(k: int) => do return s; end; "
    "      acc := acc + f1(1).Length; "
    "      s := s + \"-x\"; "
    "    end; "
    "    return acc + s.Length; "
    "  end Loop; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..4 do "
    "      total := total + C.Loop($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 216
    "  end Main; "
    "end C;";

// =====================================================================
// (E) OVER-CORRECTION GUARD — a captured string that is NEVER written keeps the
// #672/#2689 transfer model exactly: the first env inherits the escape-suppressed
// scope-end release and the second mints its own +1, so both envdtor releases are
// paired and the run is leak-free. The suppression is gated on an assignment
// existing at all, so this program's emitted refcount traffic is unchanged.
// Green before AND after by design; it fails only if the fix over-reaches.
// Per call: 3 + 3 = 6. Nine calls: 54.
// =====================================================================
const char* kTwoCapturesNeverWritten =
    "module M; class C "
    "  public static method Two(n: int): int "
    "    var s := $\"v-{n}\"; "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    var f2 := lambda(k: int) => do return s; end; "
    "    return f1(1).Length + f2(2).Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..9 do "
    "      total := total + C.Two(i); "
    "    end; "
    "    return total; "                            // 54
    "  end Main; "
    "end C;";

// =====================================================================
// (F) OVER-CORRECTION GUARD FOR THE COUNT GATE, and the reason the fix is not
// simply "suppress whenever the variable is written". ONE lambda node, NOT in a
// loop: exactly ONE env holds the cell, so one envdtor release pairs the one
// reference the slot owns — that shape is BALANCED and LEAK-FREE today
// (mechanically verified pre-fix: exit 108, LSan silent) even though the
// variable IS written after the capture. Suppressing there would strand the
// final value on every call: nine leaked strings and a red LSan. This case is
// what the `singleCaptureLocals_ && loopStack_.empty()` half of the gate exists
// to protect.
// Per call: 6 + 6 = 12. Nine calls: 108.
// =====================================================================
const char* kWriteAfterSingleCaptureNoLoop =
    "module M; class C "
    "  public static method One(p: string): int "
    "    var s := p + \"!\"; "
    "    var f1 := lambda(k: int) => do return s; end; "
    "    s := s + \"-x\"; "
    "    return f1(1).Length + s.Length; "
    "  end One; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..9 do "
    "      total := total + C.One($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 108
    "  end Main; "
    "end C;";

// =====================================================================
// (G) The FUNCTION-TYPED sibling. §28.1 bars `owned` on a function type in the
// same sentence it bars it on `string`, the envdtor releases such a capture the
// same way (`bz_closure_release`), and the capture retain is taken on the cell's
// contents there too — so re-binding a captured closure variable desynchronizes
// them identically. Pre-fix: a double free in `bz$lam$N$envdtor`.
// Plain path: closure locals passed as arguments are subject to the pre-existing
// closure-cell leak #2689 case (D) documents, so LSan cannot be the oracle here.
// Per call (p length 3): after the write `g(k)` is `k + 2*3`, and both closures
// read the CELL, so f1(1)=7, f2(2)=g(3)=9, g(3)=9 → 25. Nine calls: 225.
// =====================================================================
const char* kRebindCapturedClosureLocal =
    "module M; class C "
    "  public static method Two(p: string): int "
    "    var g: (int) -> int := lambda(k: int) => do return k + p.Length; end; "
    "    var f1 := lambda(k: int) => do return g(k); end; "
    "    var f2 := lambda(k: int) => do return g(k + 1); end; "
    "    g := lambda(k: int) => do return k + 2 * p.Length; end; "
    "    return f1(1) + f2(2) + g(3); "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..9 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 225
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) A captured LOCAL written after two captures — no parameter involved.
// §27.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_string_cell_write_2706,
           write_after_two_captures_of_local_no_double_free_spec_27_5) {
    JitResult r = runAot(kWriteAfterTwoCapturesOfLocal);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: aborted in bz$lam$0$envdtor
    BEZEL_EXPECT_EQ(r.exitCode, 162);  // both captures read the post-write value
}

// =====================================================================
// (B) The same write against a captured `string` PARAMETER — #2696's recorded
// residual. §28.1 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_string_cell_write_2706,
           write_after_two_captures_of_param_no_double_free_spec_28_1) {
    JitResult r = runAot(kWriteAfterTwoCapturesOfParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 135);
}

// =====================================================================
// (C) Leak-checked: the displaced value must not keep a stranded capture retain.
// Fails pre-fix in the LEAK direction, and would fail again if the fix had
// dropped only the envdtor release. §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_string_cell_write_2706,
           displaced_value_not_over_retained_by_capture_spec_28_9) {
    JitResult r = runAotChecked(kWriteLiteralAfterTwoCaptures);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak of the displaced string
    BEZEL_EXPECT_EQ(r.exitCode, 81);
}

// =====================================================================
// (D) One lambda node in a loop: many envs over ONE guarded cell, so the env
// count is dynamic. §27.5.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_string_cell_write_2706,
           write_in_loop_with_capture_in_loop_no_use_after_free_spec_27_5) {
    JitResult r = runAot(kWriteInLoopWithCaptureInLoop);
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: heap-use-after-free
    BEZEL_EXPECT_EQ(r.exitCode, 216);
}

// =====================================================================
// (E) Over-correction guard: a never-written capture keeps the #672 transfer
// model and stays leak-free. Green either way by design. §27.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_string_cell_write_2706,
           never_written_capture_keeps_transfer_model_spec_27_5) {
    JitResult r = runAotChecked(kTwoCapturesNeverWritten);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak from an over-broad suppression
    BEZEL_EXPECT_EQ(r.exitCode, 54);
}

// =====================================================================
// (F) Over-correction guard for the env-count gate: a sole, non-loop capture is
// balanced today even though it is written, and must not start leaking. §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_string_cell_write_2706,
           sole_non_loop_capture_of_written_local_stays_balanced_spec_28_9) {
    JitResult r = runAotChecked(kWriteAfterSingleCaptureNoLoop);
    BEZEL_EXPECT_TRUE(r.ran);          // must stay leak-free
    BEZEL_EXPECT_EQ(r.exitCode, 108);
}

// =====================================================================
// (G) The function-typed sibling: re-binding a captured closure variable.
// §28.1 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_lambda_captured_string_cell_write_2706,
           rebind_captured_closure_local_no_double_free_spec_28_1) {
    JitResult r = runAot(kRebindCapturedClosureLocal);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 225);
}
