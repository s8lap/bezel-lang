// Spec-derived regression test for issue #7484 — the §28.10 "vacuous `using
// var`" lint must NOT fire on a binding whose RIGHT-HAND SIDE is a MOVE-ONLY
// OWNING INLINE ARRAY (`[N owned]T` / `[N owned]T?`, §15.3).
//
// THE DEFECT
// ==========
// The lint (`MethodOwnership::handleLocalDecl`, src/ownership/Ownership.cpp)
// draws
//
//   "`using var` `x` is vacuous — the RHS is a value-typed/runtime-managed
//    value with no disposal obligation (§28.9); use `const x := …;` …"
//
// on
//
//   public static method Take(src: [2 owned]Res?)
//     using var x := src;
//   end Take;
//
// ⇒ AND THE LINT'S PREMISE IS FALSE AT THIS SHAPE.
//
// THE SPEC ANCHOR — §15.3 *Move-only value types* (spec
// docs/Bezel_Language_Spec.md:3038), *Local lifetime* (spec :3051), verbatim:
//
//   "a move-only local's owned slots are auto-disposed at scope end by the
//    compiler, in reverse slot order … No must-use warning attaches to this
//    auto-drop at an owning inline array (§28.3's lint, K3.14): the slot pass
//    is unconditional and runs identically under plain `var` and under `using
//    var`, so the two spellings differ in nothing the lint could announce —
//    `using var`'s only observable consequence at this kind is the
//    reassignment lock"
//
// and, in the same section's closing paragraph (spec :3079):
//
//   "`using var` is admitted for move-only inline arrays per the general rule
//    of §28.10 … For a move-only inline-array local, `using var` has no
//    disposal effect (scope-end auto-dispose runs whether or not `using var`
//    is applied) … neither draws a diagnostic — the must-use lint does not
//    attach at this kind"
//
// ⇒ THE OWNING INLINE ARRAY CARRIES A REAL DISPOSAL OBLIGATION (the scope-end
// slot pass), and §15.3 says no must-use warning attaches to it. §28.9's own
// closing paragraph (spec :8130) agrees that the runtime-managed tier does not
// reach it:
//
//   "Note that inline arrays `[N]T` (see §15.1) are not on this list. Inline
//    arrays are value types, stored inline in their enclosing context; they
//    have no separate allocation to reclaim and no disposal contract
//    independent of their host. Inline arrays with owned slots (`[N owned]T?`)
//    are move-only value types (§15.3) that participate in the ownership
//    system as part of their host's lifecycle."
//
// (§28.9's tier is EXHAUSTIVE — spec :8128: "This tier is exhaustive: no
// runtime lifetime mechanism exists for user-defined classes at all" — and it
// names `string`, `Type`, boxed value types and function values, "none of them
// is a class". An owning inline array is none of those.) ⇒ The lint's own
// message — "the RHS is a value-typed/runtime-managed value with no disposal
// obligation (§28.9)" — is FALSE at this shape: the value is a value type, but
// it DOES carry a disposal obligation.
//
// THE SITE — ONE ARM OF ONE FUNCTION
// =================================
// The lint asks `MethodOwnership::rhsIsDisposableClassType`
// (src/ownership/Ownership.cpp), whose LAST LINE reads the ONE shared
// authority:
//
//     return common::classifyDisposal(rt, ctx_).carriesObligation;
//
// `common::classifyDisposal` (src/common/DisposalKind.cpp) is the SAME
// authority the codegen registration (`Emitter::registerUsingDisposal`) reads,
// so the two sides cannot drift. Its INLINE-ARRAY arm recognised the move-only
// owning inline array (`v.kind = DisposalKind::MoveOnly` when `t->arrayOwned`)
// but never set `v.carriesObligation` — so the verdict answered "carries no
// obligation" and the lint fired. This is the sibling of #7456, which fixed
// the SAME false premise for the built-in DISPOSAL heads by deriving the
// lint's table from one shared authority.
//
// ⚠ THE CODEGEN SIDE READS THE SAME VERDICT, BUT NOT THIS FIELD.
// `classifyDisposal`'s `carriesObligation` is read at exactly ONE site outside
// its own definition — the lint (`grep -rn carriesObligation src/`). The
// codegen registration (`Emitter::registerUsingDisposal`,
// src/codegen/LlvmBackend_Setup.cpp) reads the verdict's `mayHoldClass` and
// `common::isRuntimeManagedValue(repTy)` — NOT `carriesObligation` — and the
// inline-array arm's `mayHoldClass` and `kind` are UNCHANGED by this fix. The
// fix therefore changes NOTHING the codegen emits, and the scope-end slot pass
// — the §15.3 auto-drop — runs identically. The rows below MEASURE that: the
// owned slots are disposed EXACTLY ONCE, at the scope end, on both sides of
// the fix.
//
// THE ORACLES (and why the absence rows are not vacuous)
// =======================================================
// ⚠ `runAot` fills `JitResult::diagnostics` only on a REJECT, so asserting
// the ABSENCE of a Warning on a program that COMPILES would be vacuous. The
// diagnostic rows therefore use `parseAndCheckOwnership`, the in-process
// ownership-pass oracle, which reports the pass's WARNINGS for a compiling
// program — the same oracle the §28.10 rows of #2695 / #7456 / #7523 use.
//
// The absence rows are non-vacuous because the CONTROL rows assert that the
// SAME lint, over the SAME `using var` spelling, DOES fire when the
// right-hand side really is obligation-free (a non-owning inline array, a
// value-typed binding, a non-owning choice, a borrowing collection): the
// oracle can see the warning, so its absence is a fact about the compiler,
// not about the oracle.
//
// ⚠ THE RESIDUAL — THE §28.10 BORROW-PIN LINT, A DIFFERENT RULE
// ==============================================================
// At the PLAIN parameter spelling the §28.9 vacuity lint is REPLACED by the
// §28.10 BORROW-PIN lint ("`using var` `x` pins a source that already outlives
// this scope"). That is a DIFFERENT lint with a DIFFERENT premise, and it is
// NOT part of this change's population:
//
//   * Its firing set is INDEPENDENT of `carriesObligation` — the borrow-pin
//     classifier (`MethodOwnership::rhsIsVacuousBorrowPin`) reads only the
//     initializer's SYMBOL and the parameter's `owned` modifier; it never
//     consults the verdict this change moves. So this change can only turn
//     the vacuity arm OFF; it can never turn the borrow-pin arm ON. What the
//     measurement shows is that the vacuity arm was SHADOWING the borrow-pin
//     arm at this shape.
//   * The borrow-pin arm's own premise is a SEPARATE, PRE-EXISTING defect:
//     §28.10's `using var` initialized from a borrow (spec :8252) says "an
//     unmarked read of a move-only value binding moves instead and is an
//     owned right-hand side, §15.3" — so a MOVE-ONLY parameter is not a
//     borrow and the borrow-pin rule does not reach it. That defect is
//     tracked separately as #7641 (open), whose site is
//     `MethodOwnership::rhsIsVacuousBorrowPin` in src/ownership/Ownership.cpp
//     — a file this change does not touch.
//   * THE CONTROL THAT PROVES IT IS PRE-EXISTING: the OWNING CHOICE — the
//     other move-only kind — draws the SAME borrow-pin warning at its own
//     plain parameter spelling, on the BASE, with no change of mine
//     involved. The row
//     `the_owning_inline_array_parameter_spelling_matches_the_owning_choice_
//     parameter_spelling` asserts the two shapes draw the SAME diagnostics,
//     so the inline array MATCHES the choice (the other move-only kind) —
//     and so the residual stays the choice's residual, not a new one.
//
// ⚠ THE RUNTIME ROWS ASSERT THE DISCHARGE COUNT, NOT MERELY "NO DIAGNOSTIC".
// A fix that silenced the warning but STOPPED discharging the owned slots
// would be a LEAK, not a fix; a fix that discharged them twice would be a
// DOUBLE-FREE. Both are REJECTS. Each runtime row counts the destructor
// invocations exactly, so both are caught. The leak rows use `runAotChecked`
// (ASan + LSan on the GENERATED code) and assert `hasSanitizerReport` is
// false, and the toolchain skip is asserted LOUDLY.
//
// ROWS:
//   1. REPRO        the plain `[2 owned]Res?` parameter source → NO §28.9
//                    vacuity warning (RED before the fix)
//   2. REPRO        the LOCAL source → NO diagnostic of the §28.10 family at
//                    all (RED before the fix)
//   3. REPRO        the `owned`-modified parameter spelling → NO §28.9
//                    vacuity warning (RED before the fix — see the note)
//   4. REPRO        the FIELD source → NO §28.9 vacuity warning (RED before)
//   5. REPRO        the METHOD-RESULT source → NO §28.9 vacuity warning (RED)
//   6. CONTROL      a NON-owning inline array `[2]Res`  → warning FIRES
//   7. CONTROL      a NON-owning inline array `[2]Res?` → warning FIRES
//   8. CONTROL      a plain CLASS with a destructor      → NO warning
//   9. CONTROL      a TRIVIAL class (no destructor, no `owned` field)
//                                                        → NO warning
//  10. CONTROL      a built-in disposable head (StringBuilder)
//                                                        → NO warning
//  11. CONTROL      an OWNING choice (local source)      → NO warning
//  12. CONTROL      a NON-owning choice                  → warning FIRES
//  13. CONTROL      a BORROWING built-in collection (`Array<int>`)
//                                                        → warning FIRES
//  14. CONTROL      a plain VALUE-typed binding (`5`)    → warning FIRES
//  15. CONTROL      the inline array's parameter spelling MATCHES the owning
//                    CHOICE's parameter spelling (the two move-only kinds)
//  16. RUNTIME      the owned slots are disposed EXACTLY ONCE at scope end
//  17. RUNTIME      the `owned` spelling: the same ONE discharge
//  18. RUNTIME      the same, in a loop (the LEAK row)  → no leak, 2/iteration
//  19. RUNTIME      the same array bound TWICE (a move into a `using var`)
//                                                        → one discharge
//  20. RUNTIME      an owning inline array in a CLASS FIELD → one discharge
//  21. RUNTIME      an owning inline array in an `owned` PARAMETER
//                                                        → one discharge
//
// A NOTE ON ROW 3 AND THE ISSUE'S OWN WORDING. The issue records the
// `owned`-modified spelling (`Take(owned src: [2 owned]Res?)`) as CLEAN. That
// was MEASURED at the R36 tip 59058de4 (2026-10-28). The base this change
// builds on is the R41 chain tip 6c2683c4, and the #7483 change
// (commit 1a9b86e3, 2026-11-10) unified the two parameter spellings —
// "A parameter declared `(owned x: V)` is identical to plain `(x: V)` for a
// move-only V" (§15.3 *Parameter passing*, spec :3048). On THIS base the two
// spellings are ONE shape, so BOTH drew the §28.9 vacuity warning before the
// change and NEITHER draws it after it. MEASURED, both arms, on 6c2683c4.
//
// per §15.3 / §28.9 / §28.10 / §35.4 / §28.4 / §22.5, v0.355, fix #7484

#include <algorithm>
#include <string>
#include <vector>

#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// ── the §28.10 Case-A "vacuous" warning needle ─────────────────────────────
// Stable fragments only: the lint's own words. Matching the RULE-CITING
// FRAGMENT as well as "is vacuous" rules out Case B's borrow-pin warning,
// which is a DIFFERENT lint (and must keep firing).
bool hasInertRhsWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a value-typed/runtime-managed") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff some WARNING diagnostic fires the §28.10 Case-B BORROW-PIN message
// — "`using var` `x` pins a source that already outlives this scope". This is
// the OTHER lint of the same family, and it is a DIFFERENT rule (the §28.10
// borrow-pin, not the §28.9 vacuity claim). It is asserted so the absence
// rows below can say "the shape draws NO diagnostic of this family", not
// merely "the §28.9 message is absent".
bool hasBorrowPinWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// The MESSAGE of every WARNING the ownership pass drew, sorted. Used by the
// "the inline array MATCHES the owning choice" row: two programs of the two
// move-only kinds must draw the SAME diagnostics, whatever they are — the row
// therefore stays true whether or not the residual borrow-pin warning is
// still there.
std::vector<std::string> warningMessages(const OwnershipResult& r) {
    std::vector<std::string> out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        out.push_back(d.message);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// How many times `needle` occurs in `hay` — the DISCHARGE COUNT oracle. The
// rows below count destructor invocations, so a fix that stopped discharging
// (a leak) or discharged twice (a double free) is caught by the COUNT, not
// by a boolean.
int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}

// The class whose destructor counts the discharge. `Name` distinguishes the
// slots so the ORDER is observable too (§15.3: "in reverse slot order").
const char* kResDecl =
    "public class Res(name: string) "
    "  public Name: string := name; "
    "  destructor Console.WriteLine(\"DTOR \" + Name); end destructor; "
    "end Res; ";

// The REPRO program. `param` is the parameter spelling under test — "src" or
// "owned src" (§15.3 *Parameter passing*: "A parameter declared `(owned x: V)`
// is identical to plain `(x: V)` for a move-only V").
std::string paramProgram(const std::string& param) {
    return std::string("module M; ") + kResDecl +
           "public class Sink "
           "  public static method Take(" + param + ": [2 owned]Res?) "
           "    using var x := src; "
           "  end Take; "
           "end Sink; "
           "public class Program "
           "  public static method Main(): int "
           "    var a: [2 owned]Res? := [new Res(\"x\"), new Res(\"y\")]; "
           "    Sink.Take(a); "
           "    Console.WriteLine(\"AFTER\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The NON-owning inline array — the CONTROL that must keep warning. `[N]T`
// and `[N]T?` have NO `owned` slot marker, so they are ordinary copyable
// value types with nothing to dispose (§15.3: "an inline array is move-only
// exactly when its slots carry the `owned` marker").
std::string nonOwningArrayProgram(const std::string& elemType,
                                   const std::string& literal) {
    return std::string("module M; ") + kResDecl +
           "public class Sink "
           "  public static method Take(src: " + elemType + ") "
           "    using var x := src; "
           "  end Take; "
           "end Sink; "
           "public class Program "
           "  public static method Main(): int "
           "    var r: Res := new Res(\"x\"); "
           "    var a: " + elemType + " := " + literal + "; "
           "    Sink.Take(a); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// A plain CLASS with a destructor: it carries a real obligation, so the
// lint must stay quiet (§28.4).
std::string classWithDtorProgram() {
    return "module M; "
           "public class WithDtor "
           "  public constructor() end constructor; "
           "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
           "end WithDtor; "
           "public class Program "
           "  public static method Main(): int "
           "    using var c := new WithDtor(); "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// A TRIVIAL class: no destructor and no `owned` field (§28.9's trivial-dispose
// definition). It carries no obligation OF ITS OWN — the lint's premise holds
// for it, and §28.9's trivial-dispose exemption says no must-use diagnostic
// attaches to it either.
std::string trivialClassProgram() {
    return "module M; "
           "public class Trivial "
           "  public V: int := 0; "
           "  public constructor() end constructor; "
           "end Trivial; "
           "public class Program "
           "  public static method Main(): int "
           "    using var c := new Trivial(); "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// A built-in DISPOSABLE head — the shape #7456 fixed. `StringBuilder` is
// named by §28.9's trivial-dispose list as a standard-library class with a
// disposal obligation of its own.
std::string builtinDisposableProgram() {
    return "module M; "
           "public class Program "
           "  public static method Main(): int "
           "    using var sb := new StringBuilder(); "
           "    sb.Append(\"hi\"); "
           "    Console.WriteLine(sb.ToString()); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// An OWNING choice (§35.4) — the other move-only kind. §35.4: the owning
// choice "does draw the lint at its local's auto-drop … and §35.4 states both
// the warning and its `using var` suppression" — so the `using var` spelling
// over a LOCAL source is clean.
std::string owningChoiceProgram() {
    return std::string("module M; ") + kResDecl +
           "public choice OChoice Text(owned r: Res), Empty; end OChoice; "
           "public class Program "
           "  public static method Main(): int "
           "    using var c := OChoice.Text(new Res(\"x\")); "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The OWNING CHOICE's PARAMETER spelling — the TWIN of the repro's plain
// parameter spelling, over the other move-only kind. It is the control that
// shows the residual at the plain spelling is the borrow-pin lint's
// PRE-EXISTING behaviour (it draws that warning on the BASE too), and not
// something this change introduced.
std::string owningChoiceParamProgram() {
    return std::string("module M; ") + kResDecl +
           "public choice OChoice Text(owned r: Res), Empty; end OChoice; "
           "public class Sink "
           "  public static method Take(src: OChoice) "
           "    using var x := src; "
           "  end Take; "
           "end Sink; "
           "public class Program "
           "  public static method Main(): int "
           "    Sink.Take(OChoice.Text(new Res(\"x\"))); "
           "    Console.WriteLine(\"AFTER\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// A NON-owning choice — the CONTROL. Its payload is a BORROW, so the choice
// holds no owned payload and carries no disposal obligation.
std::string nonOwningChoiceProgram() {
    return std::string("module M; ") + kResDecl +
           "public choice VChoice Text(r: Res), Empty; end VChoice; "
           "public class Program "
           "  public static method Main(): int "
           "    var r: Res := new Res(\"x\"); "
           "    using var c := VChoice.Text(r); "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// A BORROWING built-in collection — the CONTROL. §28.11: the standard
// collections "do not own their elements", so the binding carries no
// disposal obligation and the lint's premise holds for it.
std::string borrowingCollectionProgram() {
    return "module M; "
           "public class Program "
           "  public static method Main(): int "
           "    using var xs: Array<int> := [1, 2, 3]; "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The plain VALUE-typed binding — the lint's actual purpose, and the control
// that proves the lint was not silenced wholesale.
std::string valueTypedProgram() {
    return "module M; "
           "public class Program "
           "  public static method Main(): int "
           "    using var x := 5; "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The FIELD source: an owning inline array held in a class field and MOVED
// out into the `using var` binding.
std::string fieldSourceProgram() {
    return std::string("module M; ") + kResDecl +
           "public class Holder "
           "  public Slots: [2 owned]Res? := [new Res(\"x\"), new Res(\"y\")]; "
           "  public constructor() end constructor; "
           "end Holder; "
           "public class Program "
           "  public static method Main(): int "
           "    var h := new Holder(); "
           "    using var x := h.Slots; "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The METHOD-RESULT source: an owning inline array RETURNED by a method and
// bound into the `using var` binding.
std::string resultSourceProgram() {
    return std::string("module M; ") + kResDecl +
           "public class Program "
           "  public static method Make(): [2 owned]Res? "
           "    return [new Res(\"x\"), new Res(\"y\")]; "
           "  end Make; "
           "  public static method Main(): int "
           "    using var x := Make(); "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The LEAK row: 200 iterations, each moving a fresh owning inline array
// through the `using var` parameter binding. The slot pass must discharge
// every slot exactly once, and ASan/LSan must report nothing.
std::string loopProgram() {
    return std::string("module M; ") + kResDecl +
           "public class Sink "
           "  public static method Take(src: [2 owned]Res?) "
           "    using var x := src; "
           "  end Take; "
           "end Sink; "
           "public class Program "
           "  public static method Main(): int "
           "    for var i in 0..<200 do "
           "      var a: [2 owned]Res? := [new Res(\"x\"), new Res(\"y\")]; "
           "      Sink.Take(a); "
           "    end for; "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The ALIAS row: the SAME owning inline array bound twice — once at a plain
// local, once MOVED into a `using var` binding. §15.3: "Local-to-local
// assignment `var y := x;`: copies for an ordinary value type; moves for a
// move-only value type", so the source is left moved-from and only the
// `using var` binding carries the discharge — ONE discharge, not two.
std::string aliasProgram() {
    return std::string("module M; ") + kResDecl +
           "public class Program "
           "  public static method Main(): int "
           "    var a: [2 owned]Res? := [new Res(\"x\"), new Res(\"y\")]; "
           "    using var b := a; "
           "    Console.WriteLine(\"END\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The CLASS-FIELD row: an owning inline array as a class FIELD. The field's
// type-level `owned` slot marker is the obligation marker (§28.5: the
// field-level `owned` modifier would be a DOUBLE marking and is rejected);
// the generated `Dispose()` disposes each non-null slot (§28.4 step 2).
std::string ownedFieldProgram() {
    return std::string("module M; ") + kResDecl +
           "public class Holder "
           "  public Slots: [2 owned]Res? := [new Res(\"x\"), new Res(\"y\")]; "
           "  public constructor() end constructor; "
           "end Holder; "
           "public class Program "
           "  public static method Main(): int "
           "    var h := new Holder(); "
           "    Console.WriteLine(\"BEFORE\"); "
           "    h.Dispose(); "
           "    Console.WriteLine(\"AFTER\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The `owned` PARAMETER row: the move-only array handed to an `owned`
// parameter, which §15.3 makes identical to the plain spelling.
std::string ownedParamProgram() {
    return std::string("module M; ") + kResDecl +
           "public class Sink "
           "  public static method Take(owned src: [2 owned]Res?) "
           "    using var x := src; "
           "  end Take; "
           "end Sink; "
           "public class Program "
           "  public static method Main(): int "
           "    var a: [2 owned]Res? := [new Res(\"x\"), new Res(\"y\")]; "
           "    Sink.Take(a); "
           "    Console.WriteLine(\"AFTER\"); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// ============================================================================
// ROW 1 — THE REPRO. The §28.9 VACUITY warning must NOT be drawn.
//
// RED before the fix: on the pre-fix tree the shared authority answered "carries
// no obligation" for the move-only owning inline array and the lint fired.
// ============================================================================

// per §15.3 / §28.9 / §28.10, fix #7484
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owning_inline_array_parameter_source_draws_no_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(paramProgram("src"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
}

// ============================================================================
// ROW 2 — THE LOCAL-SOURCE TWIN. The owning inline array bound at a plain
// LOCAL and moved into the `using var` binding.
//
// A local is never wider-than-scope, so NEITHER lint of this family can speak
// here: the shape draws NO diagnostic at all. RED before the fix (the §28.9
// vacuity warning fired), GREEN after it.
//
// ⇒ This row is what shows the defect is the TYPE, not the source: the same
// owning inline array, bound from a local, is clean on both arms of the
// §28.10 family.
// ============================================================================

// per §15.3 / §28.9 / §28.10, fix #7484
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owning_inline_array_local_source_draws_no_diagnostic_at_all_28_10) {
    auto r = parseAndCheckOwnership(aliasProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "b"));
    BEZEL_EXPECT_FALSE(hasBorrowPinWarning(r, "b"));
}

// ============================================================================
// ROW 3 — THE `owned`-MODIFIED SPELLING. §15.3 *Parameter passing*: "A
// parameter declared `(owned x: V)` is identical to plain `(x: V)` for a
// move-only V", so the two spellings must agree.
//
// MEASURED ON THE BASE (the R41 chain tip 6c2683c4): BOTH spellings drew the
// §28.9 VACUITY warning. The issue's "the owning spelling is clean" was
// measured on the R36 tip (59058de4, 2026-10-28), BEFORE the #7483 change
// (commit 1a9b86e3 of 2026-11-10) unified the two parameter spellings —
// before that change the `owned` formal was classified as an `owned` SINK and
// took a different path. On THIS base the two spellings are ONE shape, so
// both drew the §28.9 warning before the change and NEITHER draws it after.
//
// ⇒ AT THE `owned` SPELLING THE SHAPE IS FULLY CLEAN — neither lint of the
// §28.10 family speaks. (At the PLAIN spelling the §28.9 vacuity warning is
// gone too, and the §28.10 BORROW-PIN lint — a DIFFERENT rule, keyed on the
// parameter being a borrow — takes its place. That rule's premise ("a plain
// parameter is a borrow") is false at a MOVE-ONLY parameter, which §15.3
// makes a by-value-MOVE (an owned transfer); that is the separately-tracked
// issue #7641's subject, whose site is
// `MethodOwnership::rhsIsVacuousBorrowPin` in src/ownership/Ownership.cpp —
// a file this change does not touch. ROW 15 pins the twin: the inline array
// draws exactly what the OWNING CHOICE draws, so the residual is the choice's
// residual, not a new one.)
// ============================================================================

// per §15.3 / §28.9 / §28.10, fix #7484
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owned_modified_parameter_spelling_draws_no_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(paramProgram("owned src"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasBorrowPinWarning(r, "x"));
}

// ============================================================================
// ROW 4 — THE FIELD SOURCE. An owning inline array held in a class field and
// MOVED out into the `using var` binding.
//
// ⇒ A FIELD is not wider-than-scope, so the borrow-pin rule cannot speak
// here either: the shape draws NO diagnostic of the §28.10 family at all.
// ============================================================================

// per §15.3 / §28.9 / §28.10, fix #7484
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owning_inline_array_field_source_draws_no_diagnostic_at_all_28_10) {
    auto r = parseAndCheckOwnership(fieldSourceProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasBorrowPinWarning(r, "x"));
}

// ============================================================================
// ROW 5 — THE METHOD-RESULT SOURCE. An owning inline array RETURNED by a
// method and bound into the `using var` binding.
//
// ⇒ A call result is not a borrow, so the borrow-pin rule cannot speak here
// either: the shape draws NO diagnostic of the §28.10 family at all.
// ============================================================================

// per §15.3 / §28.9 / §28.10, fix #7484
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owning_inline_array_result_source_draws_no_diagnostic_at_all_28_10) {
    auto r = parseAndCheckOwnership(resultSourceProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasBorrowPinWarning(r, "x"));
}

// ============================================================================
// THE CONTROLS — the lint must NOT be silenced wholesale.
//
// Each control asserts that the SAME lint, over the SAME `using var` spelling,
// STILL fires when the right-hand side really does carry no obligation. They
// are what make the absence rows above non-vacuous.
// ============================================================================

// per §15.1 / §15.3 / §28.9 / §28.10, fix #7484
//
// A NON-owning inline array `[2]Res`: no `owned` slot marker, so no owned
// payload, so nothing to dispose — the lint's premise HOLDS and the warning
// must still fire.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_non_owning_inline_array_still_warns_28_10) {
    auto r = parseAndCheckOwnership(nonOwningArrayProgram("[2]Res", "[r, r]"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

// per §15.1 / §15.3 / §28.9 / §28.10, fix #7484
//
// The NULLABLE non-owning inline array `[2]Res?` — the same shape, nullable.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_non_owning_nullable_inline_array_still_warns_28_10) {
    auto r = parseAndCheckOwnership(nonOwningArrayProgram("[2]Res?", "[r, null]"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

// per §28.4 / §28.10, fix #7484
//
// A plain CLASS with a destructor: it carries a real obligation, so the lint
// stays quiet — unchanged by the fix.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_class_with_a_destructor_draws_no_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(classWithDtorProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
}

// per §28.9 / §28.10, fix #7484
//
// A TRIVIAL class — no destructor, no `owned` field. MEASURED on the base: the
// lint does NOT fire here, and the fix must not change that. (The lint's
// Case-A arm requires a NON-borrowable right-hand side, and every
// class-typed right-hand side is borrow-bearing — §28.3.)
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_trivial_class_draws_no_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(trivialClassProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
}

// per §28.3 / §28.9 / §28.10, fix #7484
//
// A built-in DISPOSABLE head (`StringBuilder`) — the shape #7456 fixed. It
// must stay quiet: the fix is scoped to the inline-array arm.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_builtin_disposable_head_draws_no_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(builtinDisposableProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "sb"));
}

// per §35.4 / §28.10, fix #7484
//
// An OWNING choice — the other move-only kind. §35.4 states the warning AND
// its `using var` suppression, so the `using var` spelling is clean. The
// inline array must MATCH the choice.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_owning_choice_draws_no_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(owningChoiceProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
}

// per §35.4 / §28.10, fix #7484
//
// A NON-owning choice — its payload is a BORROW, so it carries no owned
// payload and no obligation: the lint's premise holds and it still warns.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_non_owning_choice_still_warns_28_10) {
    auto r = parseAndCheckOwnership(nonOwningChoiceProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "c"));
}

// per §28.11 / §28.9 / §28.10, fix #7484
//
// A BORROWING built-in collection (`Array<int>`) — §28.11: it does not own
// its elements, so the binding carries no obligation and the warning fires.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_borrowing_builtin_collection_still_warns_28_10) {
    auto r = parseAndCheckOwnership(borrowingCollectionProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "xs"));
}

// per §28.9 / §28.10, fix #7484
//
// The plain VALUE-typed binding — the lint's actual purpose. If this row ever
// goes quiet, the fix has silenced the lint wholesale and every absence row
// above has become vacuous.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            control_value_typed_binding_still_warns_28_10) {
    auto r = parseAndCheckOwnership(valueTypedProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

// ============================================================================
// ROW 15 — THE MATCH-THE-CHOICE CONTROL. §15.3 has exactly TWO move-only
// kinds: the owning inline array and the owning-classified choice. The two
// must agree.
//
// ⇒ The inline array's PLAIN-PARAMETER spelling must draw EXACTLY the
// diagnostics the owning CHOICE's plain-parameter spelling draws. Measured on
// the BASE, the choice's spelling already draws the §28.10 BORROW-PIN warning
// (the vacuity arm does not reach it — the choice is excluded from Case A by
// `rhsIsOwningChoice`), so the residual the inline array now shows is the
// CHOICE'S residual, and the inline array MATCHES its twin.
//
// ⇒ THE ROW IS WRITTEN AS AN EQUALITY, NOT AS AN ABSENCE: it stays true
// whether or not the borrow-pin warning is still there. That is the point —
// the borrow-pin lint's own defect (a plain move-only parameter is not a
// borrow) is a SEPARATE, separately-tracked one (#7641) whose site is in
// src/ownership/Ownership.cpp, a file this change does not touch.
// ============================================================================

// per §15.3 / §28.10 / §35.4, fix #7484
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owning_inline_array_parameter_spelling_matches_the_owning_choice_parameter_spelling) {
    auto inlineArray = parseAndCheckOwnership(paramProgram("src"));
    auto owningChoice = parseAndCheckOwnership(owningChoiceParamProgram());
    BEZEL_EXPECT_FALSE(inlineArray.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(owningChoice.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(warningMessages(inlineArray) ==
                       warningMessages(owningChoice));
}

// ============================================================================
// THE RUNTIME ORACLE — the lint's silence is NOT "there is nothing to
// dispose": the owning inline array's owned slots ARE disposed, EXACTLY ONCE,
// at the scope end (§15.3's slot pass).
//
// ⚠ A FIX THAT SILENCES THE WARNING BUT STOPS DISCHARGING THE SLOTS IS A LEAK,
// AND A FIX THAT DISCHARGES THEM TWICE IS A DOUBLE FREE. BOTH ARE REJECTS,
// so every row below COUNTS the destructor invocations.
// ============================================================================

// per §15.3 / §28.4, fix #7484
//
// The repro's program: the two owned slots are disposed exactly once each, at
// the `using var` binding's scope end, in REVERSE SLOT ORDER (§15.3: "its
// owned slots are auto-disposed at scope end by the compiler, in reverse slot
// order").
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owned_slots_are_disposed_exactly_once_at_scope_end_15_3) {
    JitResult r = runAotChecked(paramProgram("src"));
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Exactly TWO destructor runs — one per owned slot, and no more.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DTOR "), 2);
    // Reverse slot order: the second slot is disposed first.
    BEZEL_EXPECT_EQ(r.stdoutText, "DTOR y\nDTOR x\nAFTER\n");
    // ASan + LSan: nothing leaked, nothing freed twice.
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasSanitizerReport(
        r.stdoutText));
}

// per §15.3 / §28.4, fix #7484
//
// The `owned`-modified parameter spelling: the same ONE discharge.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owned_modified_spelling_disposes_the_slots_exactly_once_15_3) {
    JitResult r = runAotChecked(ownedParamProgram());
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DTOR "), 2);
    BEZEL_EXPECT_EQ(r.stdoutText, "DTOR y\nDTOR x\nAFTER\n");
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasSanitizerReport(
        r.stdoutText));
}

// per §15.3 / §28.4 / §28.9, fix #7484
//
// THE LEAK ROW. 200 iterations, each moving a fresh owning inline array
// through the `using var` parameter binding: exactly 400 destructor runs
// (2 per iteration), and ASan/LSan reports NOTHING — zero bytes leaked,
// zero double frees, zero use-after-free.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owned_slots_are_discharged_once_per_iteration_with_no_leak_15_3) {
    JitResult r = runAotChecked(loopProgram());
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DTOR "), 400);
    // The program's last line is its own "END" — the run reached the end of
    // Main rather than dying part-way through the loop.
    BEZEL_EXPECT_TRUE(r.stdoutText.size() > 4 &&
                      r.stdoutText.compare(r.stdoutText.size() - 4, 4, "END\n") == 0);
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasSanitizerReport(
        r.stdoutText));
}

// per §15.3 / §28.4, fix #7484
//
// THE DOUBLE-FREE ROW. The SAME owning inline array bound twice — once at a
// plain local, once MOVED into the `using var` binding. §15.3's move leaves
// the source moved-from, so the scope-end pass meets a live value in ONE
// binding only: ONE discharge, not two.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_array_bound_twice_is_discharged_once_15_3) {
    JitResult r = runAotChecked(aliasProgram());
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DTOR "), 2);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\nDTOR y\nDTOR x\n");
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasSanitizerReport(
        r.stdoutText));
}

// per §15.3 / §28.4 / §28.5, fix #7484
//
// THE CODECGEN-SIDE ROW (a). The owning inline array as a CLASS FIELD: the
// generated `Dispose()` disposes each non-null slot (§28.4 step 2), exactly
// once, at the holder's teardown.
//
// (The field-level `owned` MODIFIER is a DOUBLE marking and is rejected
// (§28.5) — the slot marker in the type is the only marker the field needs.)
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owning_inline_array_field_is_discharged_once_28_4) {
    JitResult r = runAotChecked(ownedFieldProgram());
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DTOR "), 2);
    BEZEL_EXPECT_EQ(r.stdoutText, "BEFORE\nDTOR y\nDTOR x\nAFTER\n");
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasSanitizerReport(
        r.stdoutText));
}

// per §15.3 / §22.3 / §28.4, fix #7484
//
// THE CODECGEN-SIDE ROW (b). The owning inline array handed to an `owned`
// PARAMETER: discharged once, at the callee's scope end.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owned_parameter_takes_the_discharge_exactly_once_15_3) {
    JitResult r = runAotChecked(ownedParamProgram());
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DTOR "), 2);
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasSanitizerReport(
        r.stdoutText));
}

// per §15.3 / §28.4, fix #7484
//
// THE FIELD-SOURCE ROW. The owning inline array MOVED out of a class field
// into the `using var` binding: ONE discharge, at the binding's scope end.
BEZEL_TEST(using_var_owning_inline_array_vacuity_7484,
            the_owning_inline_array_moved_out_of_a_field_is_discharged_once_15_3) {
    JitResult r = runAotChecked(fieldSourceProgram());
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DTOR "), 2);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\nDTOR y\nDTOR x\n");
    BEZEL_EXPECT_FALSE(bezel::test::aot_detail::hasSanitizerReport(
        r.stdoutText));
}
