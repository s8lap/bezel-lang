// Spec-derived regression test for issue #7272 (narrow) — a `using var` local
// of STRING type was never released at scope end, while the plain-`var` and
// `const` twins of the SAME program were clean.
//
// THE SPEC ANCHOR — §28.10 "`using var`" (v0.344), the runtime-managed
// paragraph, verbatim:
//
//   "`using var` accepts a value-type or runtime-managed right-hand side. For
//    ordinary value types and runtime-managed types, the disposal at scope end
//    is a no-op (these types do not require disposal per §28.9), and the only
//    effect is the reassignability restriction."
//
// ⇒ THREE requirements, all pinned here:
//
//   (1) "the disposal at scope end is a no-op" is about the DISPOSAL
//       OBLIGATION: a `string` is runtime-managed (§28.9), it has no
//       `Dispose()` to call (§8.7). It is NOT a licence to skip releasing the
//       runtime's own reference — the producer's +1 on the freshly-rendered
//       string. (Rows 1-10.)
//   (2) "and the only effect is the reassignability restriction" — the
//       `using var` spelling of a runtime-managed value must have the SAME
//       runtime lifetime as the plain-`var` spelling. One released the string
//       and the other did not: a direct contradiction of that clause.
//       (Rows 1-10, and the `var` control rows 11-13.)
//   (3) The reassignability restriction itself must SURVIVE the fix — a
//       `using var` is a scope pin (axiom A5), and the §28.10 lint that
//       reports the vacuous spelling must still fire. (Rows 16-17, 19.)
//
// §28.9 "Runtime-managed types" (v0.344) supplies the carve the fix rests on:
// "How the runtime manages their lifecycle (reference counting, region
// allocation, or other) is not part of this specification." ⇒ the SPEC does
// not impose a disposal obligation on a `string`; the OBLIGATION that the fix
// discharges is the one the compiler's OWN string-refcount discipline
// establishes for a +1 producer (the producer's +1 is transferred into the
// local's slot and dropped at scope end — `Emitter::collectStringReleaseEligible`
// / `Emitter::armStringLocalRefcount` in src/codegen/LlvmBackend_Setup.cpp).
// §8.7 supplies the other half of "no disposal obligation": "`Dispose()` is
// class-only... primitives... do not implement `IDisposable`" — a `string`
// has no `Dispose()` to call. And the producer's own contract is the slot-3
// rendering contract, documented in src/codegen/LlvmBackend_String.cpp's
// `lowerObjectToString`: "the value it hands back is a fresh `+1` the
// consumer discharges once".
//
// THE DEFECT, MEASURED (the base arm, `bezelc --sanitize=address`, LSan):
//
//   using var t := o.ToString();   ->  34 byte(s) leaked in 2 allocations
//   var         t := o.ToString();  ->  clean
//   using var t := sb.ToString();  ->  34 byte(s) leaked in 2 allocations
//   var         t := sb.ToString(); ->  clean
//   using var t := "a" + "b";      ->  35 byte(s) leaked in 2 allocations
//   var         t := "a" + "b";     ->  clean
//
// ⇒ THE THREE-SPELLING MATRIX, with the `using var` arm the odd one out.
//
// THE SITE. `Emitter::collectStringReleaseEligible` (src/codegen/
// LlvmBackend_Setup.cpp) — its candidate walk excluded every `using var`
// local (`if (v.init && !v.isUsing)`), so the local never entered
// `stringReleaseEligible_`, `armStringLocalRefcount` never armed its
// scope-end release, and the producer's +1 was never discharged. The
// `using var` declaration's OWN disposal path cannot cover it: a `string` is
// runtime-managed (§28.9), it is not a class, so `registerUsingDisposal`
// classifies nothing for it — and §28.10 says the disposal at scope end is a
// no-op for it. The compiler's own §28.10 lint agrees ("`using var` `t` is
// vacuous — the RHS is a value-typed/runtime-managed value with no disposal
// obligation (§28.9)"), and that lint is CORRECT about the OBLIGATION and is
// kept (row 16).
//
// ── THE ORACLES (and why they differ per row) ─────────────────────────────
//
// ⚠ LEAK ROWS USE THE CHECKED ORACLE. `runAot` pins `detect_leaks=0`
// (tests/support/aot_support.hpp), so a plain `runAot` row is VACUOUS for a
// leak — the program can leak and the row stays green. `runAotChecked` sets
// `detect_leaks=1` and reports a leak as a sanitizer failure, so
// `r.ran == false` is the leak verdict. The toolchain skip is asserted
// LOUDLY (`checkedToolchainSkipped`), never absorbed by a `return` — a box
// whose clang cannot link ASan has not tested the memory claim, and a silent
// skip is indistinguishable from a pass.
//
// ⚠ THE DIAGNOSTIC ROWS CANNOT USE `runAot`. `runAot` DISCARDS the
// compiler's stderr on a successful compile, so a row that inspected
// `r.diagnostics` for a warning would see NOTHING on either arm — VACUOUS.
// ⇒ The §28.10 "vacuous" warning is pinned through the OWNERSHIP PASS run
// IN-PROCESS over the test's OWN `DiagnosticEngine`
// (`bezel::test::parseAndCheckOwnership`, tests/support/ownership_support.hpp)
// — the pass that actually emits the lint. `DiagnosticEngine::report` records
// EVERY severity, so the warning lands in the test's engine and the row can
// assert its presence AND its absence.
//
// ⚠ EVERY ROW ASSERTS THE PROGRAM'S PRINTED OUTPUT. A leak row that only
// checked "no sanitizer report" would still pass on a program that printed
// the wrong text; the output assertion is what makes the row an
// over-rejection control too.
//
// ROWS (each named below):
//
//   1. LEAK          `using var` + `Object.ToString()`      -> no leak (RED before)
//   2. LEAK          `using var` + `StringBuilder.ToString()` -> no leak (RED before)
//   3. LEAK          `using var` + a `+` concatenation       -> no leak (RED before)
//   4. LEAK          the same, 100 iterations in a loop      -> no leak (RED before)
//   5. LEAK          the scope-end release on an EARLY RETURN edge
//   6. LEAK          the scope-end release on the EXCEPTION edge
//   7. LEAK          the scope-end release in a NESTED scope
//   8. LEAK          a USER class whose `ToString` MINTS a heap string
//   9. LEAK + UAF    the DOUBLE-RELEASE probe: two aliases of the same
//                    `using var` string — no leak AND no over-release
//  10. LEAK + UAF    the USE-AFTER-SCOPE probe: an alias that outlives the
//                    `using var`'s scope must still read a live string
//  11. CONTROL       the plain-`var` twin of row 1             (green both)
//  12. CONTROL       the plain-`var` twin of row 2             (green both)
//  13. CONTROL       the plain-`var` twin of row 3             (green both)
//  14. CONTROL       `using var` of a LITERAL (an immortal sentinel)
//  15. CONTROL       `using var` of a BORROW (a field read) — the borrow side
//                    of the walk is deliberately NOT extended
//  16. DIAGNOSTIC    the §28.10 "vacuous" warning STILL fires (green both)
//  17. DIAGNOSTIC    the §28.10 pin STILL rejects a reassignment (green both)
//  18. CONTROL       an OWNED `using var` (a class-typed binding) still
//                    disposes at scope end — LSan-clean IS that assertion

#include <cstdio>
#include <string>

#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ── the §28.10 "vacuous" warning needle ────────────────────────────────────
// The exact wording of the ownership pass's lint, stable fragments only:
// "`using var` `t` is vacuous" + "no disposal obligation (§28.9)".
const char* kVacuous = "is vacuous";
const char* kVacuousObligation = "no disposal obligation";

// ── the §28.10 non-reassignability needle ──────────────────────────────────
const char* kReassign = "cannot reassign `using var` local";

// ── the programs ───────────────────────────────────────────────────────────

// (1) THE OBJECT-VTABLE ROUTE. §8.5's rendering contract: the value the
// renderer hands back is a fresh `+1` the consumer discharges once (the
// slot-3 contract in src/codegen/LlvmBackend_String.cpp's
// `lowerObjectToString`), and the consumer here is the BINDING.
const char* kUsingVarObjectToString =
    "module M7272a; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var o: Object := sb; "
    "    using var t := o.ToString(); "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (2) THE STRINGBUILDER ROUTE. stdlib §3.1's `StringBuilder.ToString()`
// mints a fresh rc := 1 heap string (the same producer shape).
const char* kUsingVarSbToString =
    "module M7272b; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    using var t := sb.ToString(); "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (3) THE CONCATENATION ROUTE — the same producer arm, reached through the
// §18.5 `+` operator rather than a renderer.
const char* kUsingVarConcat =
    "module M7272c; "
    "public class C "
    "  public static method Main(): int "
    "    using var t := \"a\" + \"b\"; "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (4) THE REPEATED SHAPE. A single 34-byte leak is easy to miss; 101 of them
// (3434 bytes in 202 allocations — `0..100` is INCLUSIVE, so the body runs
// 101 times) is not.
const char* kUsingVarLoop =
    "module M7272d; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var o: Object := sb; "
    "    var n: int := 0; "
    "    for var i in 0..100 do "
    "      using var t := o.ToString(); "
    "      n := n + t.Length; "
    "    end for; "
    "    Console.WriteLine(n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (5) THE EARLY-RETURN EDGE. §28.10: "Disposal happens on every exit path:
// normal completion, `break`, `return`, or exception". The scope-end release
// must fire on the `return` edge too.
const char* kUsingVarEarlyReturn =
    "module M7272e; "
    "public class C "
    "  public static method Make(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var o: Object := sb; "
    "    do "
    "      using var t := o.ToString(); "
    "      if t.Length > 0 then "
    "        return 1; "
    "      end if; "
    "    end; "
    "    return 0; "
    "  end Make; "
    "  public static method Main(): int "
    "    Console.WriteLine(Make().ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (6) THE EXCEPTION EDGE. §31.2's `try`/`catch` — the scope-end release must
// fire when the protected block leaves by an exception.
const char* kUsingVarException =
    "module M7272f; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var o: Object := sb; "
    "    try "
    "      using var t := o.ToString(); "
    "      Console.WriteLine(t.Length.ToString()); "
    "      throw new Exception(\"boom\"); "
    "    catch e: Exception => "
    "      Console.WriteLine(\"caught\"); "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (7) THE NESTED SCOPE. A `using var` inside a `do ... end` block: the
// release must fire at the INNER block's end, not at the method's.
const char* kUsingVarNested =
    "module M7272g; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var o: Object := sb; "
    "    var n: int := 0; "
    "    do "
    "      using var t := o.ToString(); "
    "      n := n + t.Length; "
    "    end; "
    "    Console.WriteLine(n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (8) A USER CLASS whose `ToString` MINTS a heap string (§28.3's ReturnStmt
// minting) rather than returning the §8.5 type-name literal. This is the
// #429 RETURN-category producer.
const char* kUsingVarUserToString =
    "module M7272h; "
    "public class K "
    "  public override method ToString(): string "
    "    return \"user\" + \"!\"; "
    "  end ToString; "
    "end K; "
    "public class C "
    "  public static method Main(): int "
    "    var k := new K(); "
    "    using var t := k.ToString(); "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (9) THE DOUBLE-RELEASE PROBE. Two aliases of the SAME `using var` string.
// Each alias retains its own reference (a borrow local), so the scope-end
// releases must balance: the `using var`'s own +1 and each alias's retain.
// A fix that released the string TWICE would drive the refcount negative and
// abort with "refcount over-release (string)" — which the ASan arm of this
// row catches, and which LSan reports as a failure of its own.
const char* kUsingVarTwoAliases =
    "module M7272i; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var o: Object := sb; "
    "    using var t := o.ToString(); "
    "    var u := t; "
    "    var v := t; "
    "    Console.WriteLine((u.Length + v.Length).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (10) THE USE-AFTER-SCOPE PROBE. An alias that OUTLIVES the `using var`'s
// scope (assigned inside the inner block, read after it). The alias retains
// its own reference, so the string must still be alive when it is read: a
// fix that released the `using var`'s +1 without the alias's retain would
// print garbage or fault here.
const char* kUsingVarUseAfterScope =
    "module M7272j; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var o: Object := sb; "
    "    var u: string; "
    "    do "
    "      using var t := o.ToString(); "
    "      u := t; "
    "    end; "
    "    Console.WriteLine(u.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (11)(12)(13) CONTROLS — the plain-`var` twins of rows 1, 2 and 3. Green on
// BOTH arms: they are the reference behaviour the fix must reproduce, and
// they prove the fix did not widen the walk's reach.
const char* kVarObjectToString =
    "module M7272k; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var o: Object := sb; "
    "    var t := o.ToString(); "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

const char* kVarSbToString =
    "module M7272l; "
    "public class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"x\"); "
    "    var t := sb.ToString(); "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

const char* kVarConcat =
    "module M7272m; "
    "public class C "
    "  public static method Main(): int "
    "    var t := \"a\" + \"b\"; "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (14) CONTROL — a `using var` of a LITERAL. The literal is an immortal
// sentinel (rc == BZ_STRING_RC_SENTINEL), on which retain and release are
// no-ops. Green on BOTH arms.
const char* kUsingVarLiteral =
    "module M7272n; "
    "public class C "
    "  public static method Main(): int "
    "    using var t := \"lit\"; "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (15) CONTROL — a `using var` of a BORROW (a field read). The borrow side of
// the walk is deliberately NOT extended to the `using var` spelling: the
// slot owns nothing at bind (the reference belongs to the source), and
// admitting it would widen `returnedStringLocalOwnsPlusOne`'s reach. The
// shape is LSan-clean on BOTH arms, which is what makes it a control.
const char* kUsingVarBorrow =
    "module M7272o; "
    "public class K "
    "  public Tag: string; "
    "  public constructor() Tag := \"tag\"; end constructor; "
    "end K; "
    "public class C "
    "  public static method Main(): int "
    "    var k := new K(); "
    "    using var t := k.Tag; "
    "    Console.WriteLine(t.Length.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (18) CONTROL — an OWNED `using var` (a class-typed binding, which DOES
// carry a disposal obligation): its scope-end disposal must still fire.
// LSan-clean IS that assertion.
const char* kOwnedUsingVar =
    "module M7272p; "
    "public class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(7); "
    "    Console.WriteLine(a.Count.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// (19) §28.10's non-reassignability role. The `using var` spelling of a
// string must STILL be a scope pin: reassigning it is a compile error. A fix
// that dropped the pin would be as wrong as one that rejected the spelling.
const char* kUsingVarReassigned =
    "module M7272q; "
    "public class C "
    "  public static method Main(): int "
    "    using var t := \"a\" + \"b\"; "
    "    t := \"c\"; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// ── the leak oracle ────────────────────────────────────────────────────────
// Assert: the program compiled, linked, ran to completion UNDER ASan/LSan
// with NO leak, exited with `expectedExit`, and printed exactly `expectedOut`.
//
// `runAotChecked` is the CHECKED oracle: it sets `detect_leaks=1`, so a leak
// is a sanitizer failure and `r.ran` stays false. `runAot` would be
// VACUOUS here — it pins `detect_leaks=0`.
//
// The toolchain skip is asserted, not absorbed: a `return` on a SKIP would
// make an untested memory claim indistinguishable from a pass.
void expectNoLeak(const char* source, int expectedExit,
                  const char* expectedOut = nullptr) {
    JitResult r = runAotChecked(source);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "DIAG [%d]: %s\n", (int)d.severity,
                         d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, expectedExit);
    if (expectedOut) {
        std::string out;
        for (char c : r.stdoutText) {
            if (c == '\n') continue;
            out.push_back(c);
        }
        BEZEL_EXPECT_TRUE(out == expectedOut);
        if (out != expectedOut)
            std::fprintf(stderr, "STDOUT: got \"%s\", want \"%s\"\n",
                         out.c_str(), expectedOut);
    }
}

// ── the diagnostic oracle (the ownership pass, in-process) ─────────────────
// `runAot` DISCARDS a successful compile's stderr, so the §28.10 warning is
// invisible to it. This drives the OWNERSHIP PASS (the pass that emits the
// lint) over the test's OWN DiagnosticEngine, which records EVERY severity.
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

int countWarningSubstring(const OwnershipResult& r, const char* needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) ++n;
    }
    return n;
}

void dump(const OwnershipResult& r) {
    std::fprintf(stderr, "  hasErrors=%d diagnostics (%zu):\n",
                 (int)r.diagnostics.hasErrors(),
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%d] %s\n", (int)d.severity,
                     d.message.c_str());
}

}  // namespace

// ===========================================================================
// ROW 1 — LEAK: the `using var` spelling of an `Object.ToString()` binding
// must release the rendered string at scope end. RED before the fix: the
// base arm leaked 34 byte(s) in 2 allocations, so `runAotChecked` reported a
// sanitizer failure and `r.ran` was false.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_from_object_to_string_releases_it) {
    expectNoLeak(kUsingVarObjectToString, 0, "1");
}

// ===========================================================================
// ROW 2 — LEAK: the same, through the `StringBuilder` route (stdlib §3.1's
// `ToString()` mints the same rc := 1 heap string). RED before the fix:
// 34 byte(s) in 2 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_from_stringbuilder_to_string_releases_it) {
    expectNoLeak(kUsingVarSbToString, 0, "1");
}

// ===========================================================================
// ROW 3 — LEAK: the same producer arm reached through the `+` operator.
// RED before the fix: 35 byte(s) in 2 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_from_concatenation_releases_it) {
    expectNoLeak(kUsingVarConcat, 0, "2");
}

// ===========================================================================
// ROW 4 — LEAK: the REPEATED shape (101 iterations — `0..100` is inclusive).
// A single 34-byte leak is easy to miss; 101 of them are not. RED before the
// fix: 3434 byte(s) in 202 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_releases_it_in_a_loop) {
    expectNoLeak(kUsingVarLoop, 0, "101");
}

// ===========================================================================
// ROW 5 — LEAK: the scope-end release must fire on the EARLY-RETURN edge
// (§28.10: "Disposal happens on every exit path: normal completion, `break`,
// `return`, or exception"). RED before the fix: 34 byte(s) in 2 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_releases_it_on_an_early_return) {
    expectNoLeak(kUsingVarEarlyReturn, 0, "1");
}

// ===========================================================================
// ROW 6 — LEAK: the scope-end release must fire on the EXCEPTION edge.
// RED before the fix: 34 byte(s) in 2 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_releases_it_on_the_exception_path) {
    expectNoLeak(kUsingVarException, 0, "1caught");
}

// ===========================================================================
// ROW 7 — LEAK: the scope-end release must fire at the INNER block's end in
// a nested scope. RED before the fix: 34 byte(s) in 2 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_releases_it_in_a_nested_scope) {
    expectNoLeak(kUsingVarNested, 0, "1");
}

// ===========================================================================
// ROW 8 — LEAK: a USER class whose `ToString` MINTS a heap string (§28.3's
// ReturnStmt minting — the #429 RETURN-category producer), not the §8.5
// type-name literal (which is an immortal sentinel and would make the row
// vacuous). RED before the fix: 38 byte(s) in 2 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_from_user_to_string_releases_it) {
    expectNoLeak(kUsingVarUserToString, 0, "5");
}

// ===========================================================================
// ROW 9 — LEAK + DOUBLE-RELEASE PROBE: two aliases of the same `using var`
// string. Each alias retains its own reference, so the releases must
// balance. A fix that released the string twice would drive the refcount
// negative — "refcount over-release (string)", an abort the ASan arm
// catches. RED before the fix: 34 byte(s) in 2 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_with_two_aliases_is_not_double_released) {
    expectNoLeak(kUsingVarTwoAliases, 0, "2");
}

// ===========================================================================
// ROW 10 — LEAK + USE-AFTER-SCOPE PROBE: an alias that outlives the
// `using var`'s scope must still read a LIVE string. The printed length
// ("1") is the assertion that the string was not freed under the alias.
// RED before the fix: 34 byte(s) in 2 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_alias_outliving_the_scope_is_still_valid) {
    expectNoLeak(kUsingVarUseAfterScope, 0, "1");
}

// ===========================================================================
// ROW 11 — CONTROL: the plain-`var` twin of row 1. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           var_string_local_from_object_to_string_is_still_clean) {
    expectNoLeak(kVarObjectToString, 0, "1");
}

// ===========================================================================
// ROW 12 — CONTROL: the plain-`var` twin of row 2. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           var_string_local_from_stringbuilder_to_string_is_still_clean) {
    expectNoLeak(kVarSbToString, 0, "1");
}

// ===========================================================================
// ROW 13 — CONTROL: the plain-`var` twin of row 3. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           var_string_local_from_concatenation_is_still_clean) {
    expectNoLeak(kVarConcat, 0, "2");
}

// ===========================================================================
// ROW 14 — CONTROL: a `using var` of a LITERAL (an immortal sentinel, on
// which retain and release are no-ops). Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_of_a_literal_is_still_clean) {
    expectNoLeak(kUsingVarLiteral, 0, "3");
}

// ===========================================================================
// ROW 15 — CONTROL: a `using var` of a BORROW (a field read). The borrow
// side of the walk is deliberately NOT extended; the shape is clean on BOTH
// arms. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_of_a_borrow_is_still_clean) {
    expectNoLeak(kUsingVarBorrow, 0, "3");
}

// ===========================================================================
// ROW 16 — DIAGNOSTIC: the §28.10 "vacuous" warning STILL fires for the
// `using var` spelling. The lint is CORRECT about the OBLIGATION (a
// runtime-managed value has no disposal obligation, §28.9) and is kept by
// the fix — the scope-end RELEASE of the runtime's own reference is not a
// disposal obligation. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_still_warns_vacuous) {
    OwnershipResult r = parseAndCheckOwnership(kUsingVarObjectToString);
    dump(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuous), 1);
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuousObligation), 1);
}

// ===========================================================================
// ROW 17 — DIAGNOSTIC: the §28.10 "vacuous" warning does NOT fire for the
// plain-`var` twin. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           var_string_local_does_not_warn_vacuous) {
    OwnershipResult r = parseAndCheckOwnership(kVarObjectToString);
    dump(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuous), 0);
}

// ===========================================================================
// ROW 18 — CONTROL: an OWNED `using var` (a class-typed binding with a real
// disposal obligation) still disposes at scope end — LSan-clean IS that
// assertion. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           owned_using_var_still_disposes_at_scope_end) {
    expectNoLeak(kOwnedUsingVar, 0, "1");
}

// ===========================================================================
// ROW 19 — §28.10's non-reassignability role: the `using var` spelling of a
// string must STILL be a scope pin. GREEN ON BOTH ARMS.
//
// The rejection must be a REAL compile-time diagnostic with a real source
// location — not a crash, a timeout or a sanitizer report.
// ===========================================================================
BEZEL_TEST(ownership_using_var_string_local_7272,
           using_var_string_local_is_still_not_reassignable) {
    JitResult r = runAot(kUsingVarReassigned);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kReassign));
}
