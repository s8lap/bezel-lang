// Regression tests for issue #3552 — `MethodOwnership::collectLocalDepthsStmt`
// (src/ownership/Ownership_Checks.cpp) had no `ast::CaseStmt` arm, so nothing
// declared inside a statement-`case` arm body ever entered `localDepth_`.
//
// `localDepth_` is the authority every scope-escape check consults. Each of the
// four consumers guards with `td >= 0 && sd >= 0 && td < sd` and BAILS when a
// depth is unknown (the conservative -1). With arm bodies unvisited the guard
// was never satisfied for an arm-declared local, so the rejections below were
// structurally unreachable inside a `case` — they fired for the byte-identical
// `do`-`end` spelling and silently did not fire for the `case` one.
//
// The rules that were not being enforced, quoted from the spec:
//
//   §28.3 "Local variables, ownership, and borrows", sub-heading "Borrow
//   lifetime rules" (v0.286): "A borrow may not be assigned to a local whose
//   scope strictly encloses the borrow's source's scope — including flowing
//   into a loop-carried binding along a back edge when the source is declared
//   inside the loop body (`prev := cur;` where `cur` is per-iteration). The
//   scope comparison is structural and decidable".
//
//   §31.1 "Throwing", sub-heading "Exception ownership" (v0.286): "Inside a
//   catch arm, the bound exception variable (`var ex: Exception` in the arm
//   pattern) is a **borrow** of the in-flight exception; its source is the
//   runtime, and its lifetime is the arm body's. The borrow may be passed to
//   other methods (which further borrow for the duration of the call) but may
//   not be assigned to an `owned` field, returned as `owned`, or captured by a
//   closure that may outlive the arm." And: "Exception instances themselves
//   cannot escape the arm that caught them."
//
// The consequence was not a missed diagnostic but a MEMORY-SAFETY hole.
// Measured on fd39253 (the pre-fix parent of this commit), compiling each probe
// with `bezelc --sanitize=address`:
//
//   * the §31.1 closure-capture probe compiled with NO diagnostic and reported
//     `AddressSanitizer: heap-use-after-free ... READ of size 8 ... #0 in
//     bz$MP18.C$Use`;
//   * the §31.1 plain-store probe (`holder := ex;`) likewise compiled clean;
//   * the §28.3 owned-local probe compiled clean, printed 10, and
//     LeakSanitizer reported a `Direct leak of 40 byte(s) in 1 object(s)`
//     (the arm's `using var` is not reclaimed either).
//
// Each of the three positives below is therefore paired with its `do`-`end`
// A/B partner, which was rejected both before and after the fix. That pairing
// is the point of the test: the SAME program body, differing only in whether
// the enclosing block is spelled `do … end` or a `case` arm, must get the same
// verdict.
//
// OVER-REJECTION. The fix is a WIDENING — it makes four existing rejections
// newly reachable for every local declared in a `case` arm. It is not an
// over-approximation: `localDepth_` gains entries only, entries can only turn
// the conservative -1 into a real depth, and every arm body's subtree is
// numbered at `depth + 1`, so the monotonic ordering along a single nesting
// path (the only thing the consumers read) is preserved. The last four tests
// pin the shapes closest to the new boundary that must still be ACCEPTED:
// copying a value out of an arm, using an arm-local closure entirely within the
// arm, copying `ex.Message` out of a catch arm (the workaround §31.1 itself
// recommends), and capturing a local declared OUTSIDE the `case` from inside an
// arm (target depth > source depth — not an escape).

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by `!r.ran` or an
// exit code alone.
bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Compiles, links, runs; returns the program's captured stdout with trailing
// newlines trimmed (the harness's `stdoutText` keeps them; see the
// `runAotOut` note in tests/README.md). A program that did not run — a compile
// rejection or a crash — returns the sentinel, so a row that stops compiling
// fails with a legible message rather than an empty-string match.
std::string runOut(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// The §31.1 closure-capture diagnostic (Ownership_Checks.cpp, Dest::OuterLocal
// + isCatchBorrow + outerLocalIsClosureCapture).
constexpr const char* kCatchClosure =
    "caught exception `ex` may not be captured by a closure assigned to `f`";

// The §31.1 plain-store diagnostic (same branch, non-closure RHS).
constexpr const char* kCatchStore =
    "caught exception `ex` may not be assigned to `holder`";

// The §28.3 / §28.9 owned-scope-bound-local diagnostic.
constexpr const char* kOwnedLocalClosure =
    "closure capturing `r` is assigned to `f`, which outlives it";

// A class whose `DoWork` throws, plus a `Use` that reads the exception through
// a borrow — the closure body in the §31.1 positives.
const char* kThrower =
    "  private method DoWork() "
    "    throw new InvalidOperationException(\"wxyz\"); "
    "  end DoWork; "
    "  private method Use(e: Exception): int return e.Message.Length; end Use; ";

// A trivial disposable-shaped class used as the owned scope-bound local in the
// §28.3 rows.
const char* kBox =
    "class Box public method N(): int return 10; end N; end Box; ";

}  // namespace

// =====================================================================
// §31.1 — the catch binding captured by a closure assigned to an outer local.
// A/B pair: `case` arm (the bug) vs `do`-`end` (always rejected).
// =====================================================================

// POSITIVE. The `try`/`catch` sits inside a `case` arm; `f` is declared before
// the `case`, so the closure outlives the arm. Pre-fix: compiled clean and
// ASan reported heap-use-after-free in `bz$MP18.C$Use`.
BEZEL_TEST(case_arm_local_depth_3552, catch_closure_capture_in_case_arm_rejected_31_1) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string("module M; class C ") + kThrower +
        "  public method Handle(n: int): int "
        "    var f: () -> int := lambda() => 0; "
        "    case n of "
        "      0 => do "
        "        try DoWork(); "
        "        catch var ex: Exception => do "
        "          f := lambda() => Use(ex); "
        "        end; "
        "        end; "
        "      end, "
        "      _ => do end "
        "    end case; "
        "    return f() + f(); "
        "  end Handle; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    Console.WriteLine(c.Handle(0).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        kCatchClosure));
}

// A/B PARTNER. The identical body with the arm spelled `do`-`end`. Rejected
// before the fix too (fix #3520 made this channel reachable); it is here so a
// future regression that re-breaks only the `case` half is visibly a PAIR
// failure, not a single unexplained row.
BEZEL_TEST(case_arm_local_depth_3552, catch_closure_capture_in_do_end_rejected_31_1) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string("module M; class C ") + kThrower +
        "  public method Handle(n: int): int "
        "    var f: () -> int := lambda() => 0; "
        "    do "
        "      try DoWork(); "
        "      catch var ex: Exception => do "
        "        f := lambda() => Use(ex); "
        "      end; "
        "      end; "
        "    end; "
        "    return f() + f(); "
        "  end Handle; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    Console.WriteLine(c.Handle(0).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        kCatchClosure));
}

// =====================================================================
// §31.1 — the plain-store channel: `holder := ex;` where `holder` is declared
// before the `case`. No closure involved; the borrow itself escapes.
// =====================================================================

// POSITIVE. Pre-fix: compiled clean, ASan reported heap-use-after-free.
BEZEL_TEST(case_arm_local_depth_3552, catch_plain_store_in_case_arm_rejected_31_1) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string("module M; class C ") + kThrower +
        "  public method Handle(n: int): int "
        "    var holder: Exception? := null; "
        "    case n of "
        "      0 => do "
        "        try DoWork(); "
        "        catch var ex: Exception => do "
        "          holder := ex; "
        "        end; "
        "        end; "
        "      end, "
        "      _ => do end "
        "    end case; "
        "    if holder <> null then return holder!.Message.Length; end; "
        "    return 0; "
        "  end Handle; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    Console.WriteLine(c.Handle(0).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        kCatchStore));
}

// =====================================================================
// §28.3 / §28.9 — the pre-existing half (#861 family): an owned scope-bound
// local declared in an arm, captured by a closure held in an enclosing block.
// =====================================================================

// POSITIVE. Pre-fix: compiled clean, printed 10, LeakSanitizer reported a
// 40-byte direct leak (the arm's `using var` is never reclaimed).
BEZEL_TEST(case_arm_local_depth_3552, owned_local_closure_capture_in_case_arm_rejected_28_3) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string("module M; ") + kBox +
        "class C "
        "  public method Handle(k: int): int "
        "    var f: () -> int := lambda() => 0; "
        "    case k of "
        "      0 => do "
        "        using var r := new Box(); "
        "        f := lambda() => r.N(); "
        "      end, "
        "      _ => do end "
        "    end case; "
        "    return f(); "
        "  end Handle; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    Console.WriteLine(c.Handle(0).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        kOwnedLocalClosure));
}

// A/B PARTNER — the `do`-`end` spelling, rejected on both sides of the fix.
BEZEL_TEST(case_arm_local_depth_3552, owned_local_closure_capture_in_do_end_rejected_28_3) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string("module M; ") + kBox +
        "class C "
        "  public method Handle(k: int): int "
        "    var f: () -> int := lambda() => 0; "
        "    do "
        "      using var r := new Box(); "
        "      f := lambda() => r.N(); "
        "    end; "
        "    return f(); "
        "  end Handle; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    Console.WriteLine(c.Handle(0).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;",
        kOwnedLocalClosure));
}

// =====================================================================
// NEGATIVE CONTROLS — the over-rejection boundary. Every one of these must
// still COMPILE, RUN, and produce its value.
// =====================================================================

// A value copied out of an arm into an enclosing-block local is not a borrow;
// nothing escapes. Must still be accepted.
BEZEL_TEST(case_arm_local_depth_3552, value_copy_out_of_case_arm_accepted) {
    BEZEL_EXPECT_EQ(
        "5",
        runOut(
            "module M; class C "
            "  public method Handle(k: int): int "
            "    var s: int := 0; "
            "    case k of "
            "      0 => do var t := 5; s := t; end, "
            "      _ => do end "
            "    end case; "
            "    return s; "
            "  end Handle; "
            "  public static method Main(): int "
            "    var c := new C(); "
            "    Console.WriteLine(c.Handle(0).ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"));
}

// An owned arm-local captured by a closure that is BOTH declared and invoked
// inside the same arm never outlives its source. §28.3's rule is about a target
// whose scope strictly encloses the source's; here they are the same scope.
BEZEL_TEST(case_arm_local_depth_3552, arm_local_closure_used_within_arm_accepted) {
    BEZEL_EXPECT_EQ(
        "10",
        runOut(
            std::string("module M; ") + kBox +
            "class C "
            "  public method Handle(k: int): int "
            "    var s: int := 0; "
            "    case k of "
            "      0 => do "
            "        using var r := new Box(); "
            "        var g: () -> int := lambda() => r.N(); "
            "        s := g(); "
            "      end, "
            "      _ => do end "
            "    end case; "
            "    return s; "
            "  end Handle; "
            "  public static method Main(): int "
            "    var c := new C(); "
            "    Console.WriteLine(c.Handle(0).ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"));
}

// §31.1's own recommended workaround, inside a `case` arm: "copy the relevant
// data out of it (typically `ex.Message` ...) into separate owned values".
// `ex.Message` is a `string` (§31.4), a runtime-managed value — no borrow of
// the exception escapes. Must still be accepted.
BEZEL_TEST(case_arm_local_depth_3552, catch_message_copy_out_of_case_arm_accepted) {
    BEZEL_EXPECT_EQ(
        "boom",
        runOut(
            "module M; class C "
            "  private method DoWork() "
            "    throw new InvalidOperationException(\"boom\"); "
            "  end DoWork; "
            "  public method Handle(k: int): string "
            "    var msg: string := \"\"; "
            "    case k of "
            "      0 => do "
            "        try DoWork(); "
            "        catch var ex: Exception => do msg := ex.Message; end; "
            "        end; "
            "      end, "
            "      _ => do end "
            "    end case; "
            "    return msg; "
            "  end Handle; "
            "  public static method Main(): int "
            "    var c := new C(); "
            "    Console.WriteLine(c.Handle(0)); "
            "    return 0; "
            "  end Main; "
            "end C;"));
}

// The DIRECTION guard: the source local is declared OUTSIDE the `case` and the
// closure holder is too, so the target's depth is not smaller than the source's
// and there is no escape. This is the row that would fail first if the new arm
// numbered depths in the wrong direction.
BEZEL_TEST(case_arm_local_depth_3552, outer_local_captured_from_case_arm_accepted) {
    BEZEL_EXPECT_EQ(
        "10",
        runOut(
            std::string("module M; ") + kBox +
            "class C "
            "  public method Handle(k: int): int "
            "    using var r := new Box(); "
            "    var f: () -> int := lambda() => 0; "
            "    case k of "
            "      0 => do f := lambda() => r.N(); end, "
            "      _ => do end "
            "    end case; "
            "    return f(); "
            "  end Handle; "
            "  public static method Main(): int "
            "    var c := new C(); "
            "    Console.WriteLine(c.Handle(0).ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"));
}
