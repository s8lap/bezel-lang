// Regression test for #8376 — a MOVE-ONLY INLINE-ARRAY PARAMETER carries no
// discharge obligation, so its owned slots leak.
//
// THE DEFECT (measured on the base, `r30-c1` = 9d1d3e1a4).
//
// A move-only OWNING inline array (`[N owned]T?` / `[N owned]T`) passed to a
// move-only formal LEAKED its owned slots:
//
//   module M;
//   public class Res
//     public static Count: int := 0;
//     public constructor() end constructor;
//     destructor Res.Count := Res.Count + 1; end destructor;
//   end Res;
//   public class Sink
//     public static method TakeArr(e: [1 owned]Res?) Console.WriteLine("took"); end TakeArr;
//   end Sink;
//   public class C
//     public static method T()
//       var a: [1 owned]Res? := [new Res()];
//       Sink.TakeArr(a);
//     end T;
//     public static method Main(): int
//       for var i in 0..<100 do T(); end;
//       Console.WriteLine($"{Res.Count}");
//       return 0;
//     end Main;
//   end C;
//
// prints `0` — none of the 100 `Res` objects is ever disposed — and
// LeakSanitizer reports "Direct leak of 1600 byte(s) in 100 object(s)". The
// leak is SILENT (no sanitizer report from the program itself, because nothing
// is freed twice).
//
// THE SPEC AUTHORITY.
//
//   * §15.3 *Parameter passing*: "a parameter declared `(x: V)` for an ordinary
//     V is by value (copy); for a move-only V, it is by value-move (transfer
//     ownership). The caller's argument expression is in moved-from state after
//     the call." ⇒ the CALLER's binding is moved-from and disposes nothing;
//     the CALLEE owns the slots.
//   * §15.3 *Local lifetime*: "a move-only local's owned slots are auto-
//     disposed at scope end by the compiler, in reverse slot order (§15.1.1)"
//     — and the auto-drop is UNCONDITIONAL ("No must-use warning attaches to
//     this auto-drop at an owning inline array (§28.3's lint, K3.14): the slot
//     pass is unconditional and runs identically under plain `var` and under
//     `using var`"). The callee's parameter binding is a LIVE move-only
//     binding (it is not moved-from — the CALLER's is), so the slot pass is
//     owed to the CALLEE's frame.
//   * §28.1 clause 1: "(owned x: V) is identical to plain (x: V) for a move-only
//     V" ⇒ the `owned` modifier is IRRELEVANT to this defect, and a fix that
//     only handled the `owned e: [1 owned]Res?` spelling would NOT fix #8376.
//   * §28.3: the auto-drop runs only on a live binding "not in moved-from
//     state" — the callee's parameter is live.
//
// THE FIX (src/codegen/LlvmBackend_Setup.cpp — ONE file).
//
// `Emitter::registerUnwindOwnedParam` — the callee-side parameter registration
// reached from `lowerScopedBlock`'s root-scope drain — had NO branch for a
// move-only inline-array parameter: the slot fell through every check and was
// silently skipped. It now has one, mirroring the OWNING-CHOICE parameter arm
// that is already there (the reason that twin did not leak):
//
//   * the branch is placed BEFORE the `if (ps->modifier != "owned") return;`
//     gate, because the modifier is redundant for a move-only V (§28.1 clause
//     1) — a branch after that gate would be DEAD CODE for the plain
//     `(e: [1 owned]T?)` spelling, which is the spelling that leaked;
//   * `releaseOnUnwindOnly` is left FALSE (the owning-choice arm's
//     `!ps->autoDropAtScopeEnd` CANNOT be copied: `autoDropAtScopeEnd` is set
//     only for an `owned` parameter, so copying it would make the disposal
//     fire only on the unwind edge and the NORMAL exit — the path this issue's
//     repro takes — would still leak);
//   * the slot count comes from the ALLOCA, the same derivation the LOCAL path
//     (`registerInlineOwnedArrayDisposal`) uses;
//   * the registration lands in `disposeStack_.front()` (the ROOT scope) — a
//     parameter lives for the whole method, so it must not die at an inner
//     scope end;
//   * it carries the local path's own UNKNOWN-TRANSFER guard
//     (`currentFn_ != moveOnlyArrayTransferredFn_`), so a body the whole-body
//     transfer walk never analysed is skipped (a safe leak, never a
//     double-free).
//
// The MOVE-OUT half of the fix is the transfer walk:
// `MoveOnlyArrayTransferWalker::markIdent` used to `dynamic_cast<
// sema::LocalSymbol*>` — which FAILS for a parameter (a parameter is a
// `sema::ParameterSymbol*`) — so the parameter was never recorded as
// transferred. It now admits a parameter symbol, so a callee that MOVES the
// parameter out (a return, a pass-on to another callee) is not disposed by the
// source: the receiver owns the slots.
//
// THE ROWS BELOW.
//
//   * the primary row — the issue's own repro: 100 objects, 100 dispositions,
//     LeakSanitizer clean;
//   * the §28.1 clause-1 twin — the `owned e: [1 owned]Res?` spelling: the
//     SAME count as the plain spelling (the two spellings are identical for a
//     move-only V);
//   * the CONTROL rows — the array is only a LOCAL (100), and the callee
//     COPIES the parameter into a local (100): both must be unaffected;
//   * the MOVED-OUT rows — the callee RETURNS the parameter, and the callee
//     PASSES IT ON to a second callee: both must dispose exactly once (a
//     double-free is a REJECT, not a leak).
//
// The count is asserted through the DESTRUCTOR COUNTER (the primary
// measurement) and through LeakSanitizer (the second): a one-shot leak row is
// a vacuous oracle, so every row is amplified 100×.
//
// per §15.3, §28.1, §28.3, §15.1.1, #8376

#include <cstddef>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"          // runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        // A toolchain skip and a real finding are DIFFERENT outcomes, and only
        // the former may be returned past. "runAotChecked: SKIP — C toolchain
        // (clang) ..." means the sanitizer never ran; "sanitizer fired" means it
        // ran and found something. Returning true for the second would discard
        // a genuine leak as if the box merely lacked clang — the #3733 silent
        // -skip defect, and the reason this predicate is not a bare find("SKIP").
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// The counting class: every instance bumps `Count` on disposal, and the
// destructor also prints a marker so the count is observable in the captured
// stdout (the same oracle shape as owning_choice_param_leak_test.cpp).
//
// `Count` is a mutable static primitive field, which §22.12 admits with a
// declaration-site warning — the warning is irrelevant to these rows (the
// program is a single-domain test program) and does not affect the count.
const char* kRes =
    "public class Res "
    "  public static Count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.Count := Res.Count + 1; "
    "  Console.WriteLine(\"RES-DISPOSED\"); "
    "end destructor; "
    "end Res; ";

}  // namespace

// =====================================================================
// §15.3 *Parameter passing* + *Local lifetime*: the CALLEE owns a
// move-only inline-array parameter's slots and discharges them at its
// scope end. This is the issue's own repro, amplified 100×.
// =====================================================================
//
// WITHOUT the fix: 0 dispositions, LeakSanitizer "Direct leak of 1600
// byte(s) in 100 object(s)".
// WITH the fix: 100 dispositions, no leak report.
BEZEL_TEST(ownership_moveonly_inline_array_param_8376,
           move_only_inline_array_param_disposed_at_callee_scope_end) {
    std::string src =
        "module M; " + std::string(kRes) +
        "public class Sink "
        "  public static method TakeArr(e: [1 owned]Res?) "
        "    Console.WriteLine(\"took\"); "
        "  end TakeArr; "
        "end Sink; "
        "public class C "
        "  public static method T() "
        "    var a: [1 owned]Res? := [new Res()]; "
        "    Sink.TakeArr(a); "
        "  end T; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do T(); end; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    // #3733: a silent `return` here would report a clean PASS on a box whose
    // ASan link is unavailable — green while asserting nothing about memory
    // safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip, not a failure

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    // The destructor fires exactly once per construction (100×).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    // And the printed counter agrees.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("\n100\n") != std::string::npos);

    // LeakSanitizer must report zero leaks with the fix in place.
    BEZEL_EXPECT_EQ(r.stdoutText.find("detected memory leaks"), std::string::npos);
}

// =====================================================================
// §28.1 CLAUSE 1: "(owned x: V) is identical to plain (x: V) for a
// move-only V". The `owned`-modified spelling must reach the SAME
// branch and produce the SAME count — a fix that only handled the
// `owned` spelling would leave the issue's own repro (the PLAIN
// spelling) leaking, and a fix that only handled the plain spelling
// would leave this row leaking.
// =====================================================================
BEZEL_TEST(ownership_moveonly_inline_array_param_8376,
           owned_modified_spelling_disposes_exactly_like_the_plain_spelling) {
    std::string src =
        "module M; " + std::string(kRes) +
        "public class Sink "
        "  public static method TakeArr(owned e: [1 owned]Res?) "
        "    Console.WriteLine(\"took\"); "
        "  end TakeArr; "
        "end Sink; "
        "public class C "
        "  public static method T() "
        "    var a: [1 owned]Res? := [new Res()]; "
        "    Sink.TakeArr(a); "
        "  end T; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do T(); end; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(r.stdoutText.find("detected memory leaks"), std::string::npos);
}

// =====================================================================
// CONTROL A — the array is only a LOCAL. The scope-end slot pass runs
// and has always run; this row must NOT move (a fix that changed it
// would be discharging something twice).
// =====================================================================
BEZEL_TEST(ownership_moveonly_inline_array_param_8376,
           control_local_only_still_disposes_exactly_once) {
    std::string src =
        "module M; " + std::string(kRes) +
        "public class C "
        "  public static method T() "
        "    var a: [1 owned]Res? := [new Res()]; "
        "    Console.WriteLine(\"made\"); "
        "  end T; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do T(); end; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(r.stdoutText.find("detected memory leaks"), std::string::npos);
}

// =====================================================================
// CONTROL B — the callee COPIES the parameter into a local. The local's
// scope-end pass disposes the slots; this row must NOT move.
// =====================================================================
BEZEL_TEST(ownership_moveonly_inline_array_param_8376,
           control_copied_into_a_local_still_disposes_exactly_once) {
    std::string src =
        "module M; " + std::string(kRes) +
        "public class Sink "
        "  public static method TakeArrLocal(e: [1 owned]Res?) "
        "    var x := e; "
        "    Console.WriteLine(\"took\"); "
        "  end TakeArrLocal; "
        "end Sink; "
        "public class C "
        "  public static method T() "
        "    var a: [1 owned]Res? := [new Res()]; "
        "    Sink.TakeArrLocal(a); "
        "  end T; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do T(); end; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(r.stdoutText.find("detected memory leaks"), std::string::npos);
}

// =====================================================================
// MOVED-OUT — the callee RETURNS the parameter. §15.3 *Parameter
// passing*: the value is transferred to the caller, so the callee must
// NOT discharge it and the CALLER's binding must. A double-free here
// is a REJECT (the whole point of the transfer-walk half of the fix):
// without the walker's parameter arm the callee would dispose the
// returned slots as well.
// =====================================================================
BEZEL_TEST(ownership_moveonly_inline_array_param_8376,
           returned_parameter_is_disposed_exactly_once_by_the_receiver) {
    std::string src =
        "module M; " + std::string(kRes) +
        "public class Sink "
        "  public static method TakeArr(e: [1 owned]Res?): [1 owned]Res? "
        "    return e; "
        "  end TakeArr; "
        "end Sink; "
        "public class C "
        "  public static method T() "
        "    var a: [1 owned]Res? := [new Res()]; "
        "    var b: [1 owned]Res? := Sink.TakeArr(a); "
        "    Console.WriteLine($\"{b.Length}\"); "
        "  end T; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do T(); end; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Exactly once — 200 would be a double-free, 0 a leak.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(r.stdoutText.find("detected memory leaks"), std::string::npos);
}

// =====================================================================
// MOVED-OUT — the callee PASSES THE PARAMETER ON to a second callee.
// The intermediate frame must not discharge it (the final receiver
// owns it), and the final receiver must discharge it exactly once.
// =====================================================================
BEZEL_TEST(ownership_moveonly_inline_array_param_8376,
           passed_on_parameter_is_disposed_exactly_once_by_the_final_receiver) {
    std::string src =
        "module M; " + std::string(kRes) +
        "public class Sink2 "
        "  public static method TakeArr(e: [1 owned]Res?) "
        "    Console.WriteLine(\"took\"); "
        "  end TakeArr; "
        "end Sink2; "
        "public class Sink "
        "  public static method TakeArr(e: [1 owned]Res?) "
        "    Sink2.TakeArr(e); "
        "  end TakeArr; "
        "end Sink; "
        "public class C "
        "  public static method T() "
        "    var a: [1 owned]Res? := [new Res()]; "
        "    Sink.TakeArr(a); "
        "  end T; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do T(); end; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(r.stdoutText.find("detected memory leaks"), std::string::npos);
}

// =====================================================================
// CONTROL C — a PLAIN (non-owning) inline-array parameter (`[1]Res`,
// no `owned` element modifier). Such an array is COPYABLE and owns
// nothing, so the callee must NOT discharge anything: the element is
// owned by the caller's `using var`. A fix that keyed on
// `ast::TypeKind::InlineArray` without the `arrayOwned` test would
// dispose the caller's object here — a double-free, not a leak.
// =====================================================================
BEZEL_TEST(ownership_moveonly_inline_array_param_8376,
           control_plain_non_owning_array_parameter_owns_nothing) {
    std::string src =
        "module M; " + std::string(kRes) +
        "public class Sink "
        "  public static method TakeArr(e: [1]Res) "
        "    Console.WriteLine(\"took\"); "
        "  end TakeArr; "
        "end Sink; "
        "public class C "
        "  public static method T() "
        "    using var r := new Res(); "
        "    var a: [1]Res := [r]; "
        "    Sink.TakeArr(a); "
        "  end T; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do T(); end; "
        "    Console.WriteLine($\"{Res.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED"), 100);
    BEZEL_EXPECT_EQ(r.stdoutText.find("detected memory leaks"), std::string::npos);
}
