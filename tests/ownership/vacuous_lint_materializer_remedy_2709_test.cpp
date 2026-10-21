// Ownership / diagnostics — the #2073 vacuous-`using var` lint told the author
// to write a `const` binding that COMPILES and LEAKS. Issue #2709, the surviving
// half of #2695.
//
// Spec, v0.240:
//   §21.1 "Runtime `const` locals", the **Ownership** paragraph, verbatim:
//     "The initializer of a runtime `const` must not be an **owned production**
//      (§28.3): the binding never takes a disposal obligation and never holds an
//      affine (consumable) value. […] Rejected, with a diagnostic naming this
//      rule and the `using var` remedy: a fresh `new`, an `owned`-returning
//      call, a `Task`-family call, a deferred-activation producer (a lazy query,
//      `Drain()`, `ReceiveAll()` — affine values, §29.11), a `move`, a marked
//      transfer (`const y := owned x;` is not a form), and a move-only value
//      (§15.3)."
//   stdlib §5 (LINQ), verbatim: "`ToArray()` returns a fresh `Array<T>`
//     containing the materialized sequence. The return is `owned` — the caller
//     is responsible for disposing it." Declared signatures: `ToArray(): owned
//     Array<T>`, `ToSet(): owned Set<T>`, `ToDictionary<TKey>(…): owned
//     Dictionary<TKey, T>`. stdlib §4.4 gives `Sorted`/`Reversed`/`Slice` the
//     same `owned Array<T>` return; §9.6 gives `string.Split`; §7 gives
//     `Encoding.GetBytes`.
//
// So a builtin materializer call IS "an `owned`-returning call" and §21.1
// REQUIRES its rejection as a `const` initializer.
//
// THE BUG, measured on 2a990de. `producerState` reaches its `owned`-return
// authority through a USER `MethodDecl`; these are compiler-known members with
// none, so all eight fell through to `Borrowed`. Consequences, both reproduced:
//
//   using var m := xs.Where(…).ToArray();   → WARNS "vacuous … use `const x := …;`"
//   const     m := xs.Where(…).ToArray();   → ACCEPTED  ← §21.1 says reject
//
// and the accepted spelling leaks: a `const` local is neither a `LocalDecl`
// (codegen's plain-`var` reclaim, `isFreshBorrowingArrayMaterializerLocal`) nor
// a `using var` (`registerUsingDisposal`), so NOTHING frees it. Amplified probe,
// 100 iterations per materializer, LSan: exactly 100 leaked objects each, from
// distinct allocation sites — `bz_lazy_to_array`, `bz_lazy_to_set`,
// `bz_lazy_to_dict`, `bz_array_copy` (×2: Sorted, Reversed), `bz_str_split_str`,
// and (measured separately) `Slice` and `Encoding.GetBytes`. The lint was
// steering authors straight into it.
//
// THE FIX, and why it is NOT a reclassification. `producerState`'s `Borrowed`
// verdict here is load-bearing: it is the free-deciding input at
// `handleLocalDecl`, and codegen ALREADY reclaims both bindable spellings on its
// own recognition. Flipping it to `Owned` would register a SECOND free on top —
// the double-free direction. Instead §21.1's const rejection and the lint guard
// both now route through ONE diagnostic-only predicate,
// `MethodOwnership::constInitializerRejected`, whose third disjunct is
// `common::isBuiltinOwnedMaterializerCall`. No disposal, retain, or dataflow
// state changes; the generated code for every program that still compiles is
// what it was. The leak goes away because §21.1 removes the program.
//
// The same routing subsumes #2146's separately-maintained `rhsIsDrainAliasPin`
// (issue #2709's "leg 1": the loop was closed there only by coincidence, both
// sides having happened to spell the identical predicate).
//
// KNOWN GAPS, recorded deliberately (all filed as #2749):
//   * The member list is NOT exhaustive. Grepping the stdlib for `owned Array`
//     returns also finds ToCharArray, ToByteArray, ToBytes, CopyToArray,
//     ReadAll/ReadAllBytes/ReadAllLines, GetFiles and GetDirectories. Each was
//     left out because it was not individually MEASURED, and adding a name
//     without confirming its `const` binding actually leaks would be an
//     unverified rejection of legal code.
//   * §28.3's producers table also has a "Range slice `arr[r]` on `Array<T>` →
//     Owned" row. `arr[0..2]` is an `IndexExpr`, structurally outside a
//     CallExpr-keyed predicate; only the `Slice(r)` spelling is covered here.
//   * A `const` inside a LAMBDA BODY never reaches the rejection at all —
//     `applyUsingVarBorrowPinInLambdaStmt` handles `LocalConstDecl` only to
//     recurse into nested lambdas — so `const m := xs.ToArray();` in a lambda
//     still compiles and still leaks. Pre-existing for all three §21.1
//     categories, not introduced or fixed here.
//   * The three disjuncts have inconsistent expression depth: `producerState`
//     peels a `BindExpr`, the two CallExpr disjuncts do not, so a
//     binding-atom-wrapped materializer evades the rejection. Missing-rejection
//     only — both sides agree, so no remedy loop reopens.

#include <string>

#include "support/aot_support.hpp"       // runAot / runAotChecked
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The inert-RHS ("Case A") vacuous warning, matched on its TEXT — the coarse
// has-a-diagnostic check cannot tell it from any other diagnostic.
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

// The vacuous-borrow-pin ("Case B") warning — it carries the SAME `const`
// remedy, so the suppression must not merely push Case A's shapes into it.
bool hasVacuousBorrowPinWarning(const OwnershipResult& r,
                                const std::string& name) {
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

// §21.1's rejection, in its NEW materializer wording. Matching the specific
// text (not just "must not take an owned production", which the pre-existing
// `new`-RHS branch also emits) is what pins this fix rather than #2695's.
bool hasConstMaterializerError(const OwnershipResult& r,
                               const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("must not take an owned production") ==
            std::string::npos)
            continue;
        if (d.message.find("a collection materializer returns a fresh "
                           "`owned` collection") == std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// §21.1's const rejection for Drain.  fix #4795 (§28.3 row 2, v0.307)
// updated the diagnostic text check: with Drain classified as Owned (mints
// fresh frame, per stdlib §4.5 / §4.13 v0.307), the error is the
// "owned production" form; the v0.195 "deferred-activation producer" form
// was based on the now-superseded "Drain returns the array itself" premise.
// Both error texts correctly reject the const binding; this helper accepts
// either. // per §21.1, §28.3, #4795
bool hasConstDrainError(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("must not take a deferred-activation producer") ==
            std::string::npos &&
            d.message.find("must not take an owned production") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// A single-class unit whose `M()` body is `body`, with an `Array<int>` `a` in
// scope carrying three elements.
std::string prog(const std::string& body) {
    return "module M; class C "
           "  public static method M(): int "
           "    using var a := new Array<int>(); "
           "    a.Add(3); a.Add(1); a.Add(2); " + body +
           "    return 0; end M; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

// The stdlib-declared `owned`-returning materializers, as an initializer
// expression over the `a` above. Every entry was measured leaking 100-of-100
// under `const` before being added — see the file header.
const char* kMaterializers[] = {
    "a.Where(lambda(x) => x > 0).ToArray()",           // stdlib §5
    "a.Where(lambda(x) => x > 0).ToSet()",             // stdlib §5
    "a.Where(lambda(x) => x > 0).ToDictionary(lambda(x) => x)",  // stdlib §5
    "a.Sorted()",                                      // stdlib §4.4
    "a.Reversed()",                                    // stdlib §4.4
    "a.Slice(0..2)",                                   // stdlib §4.4
    "\"a,b,c\".Split(\",\")",                          // stdlib §9.6
    "Encoding.UTF8.GetBytes(\"hello\")",               // stdlib §7
};

}  // namespace

// =====================================================================
// (1) THE #2709 REPRO — the remedy loop, both spellings, on `.ToArray()`.
// The lint's advice must compile; here it must therefore not be given, because
// §21.1 rejects it. FAILS WITHOUT THE FIX in BOTH halves (pre-fix the warning
// fired and the `const` was silently accepted).
// =====================================================================
BEZEL_TEST(vacuous_lint_materializer_2709,
           toarray_remedy_loop_closed_spec_21_1_stdlib_5) {
    auto pinned = parseAndCheckOwnership(
        prog("using var m := a.Where(lambda(x) => x > 0).ToArray();"));
    BEZEL_EXPECT_FALSE(pinned.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(pinned, "m"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(pinned, "m"));

    // The spelling the lint used to recommend: §21.1 rejects it by name.
    auto remedy = parseAndCheckOwnership(
        prog("const m := a.Where(lambda(x) => x > 0).ToArray();"));
    BEZEL_EXPECT_TRUE(hasConstMaterializerError(remedy, "m"));
}

// =====================================================================
// (2) THE WHOLE MATERIALIZER FAMILY, both directions at once. A per-method
// allowlist that fixed only `.ToArray()` would leave the identical loop open
// one method over — all six are `owned`-returning per stdlib §5 / §4.4 / §9.6,
// and all six were measured leaking under `const`.
// FAILS WITHOUT THE FIX in both halves, for every entry.
// =====================================================================
BEZEL_TEST(vacuous_lint_materializer_2709,
           materializer_family_const_rejected_and_pin_silent_spec_21_1) {
    for (const char* mat : kMaterializers) {
        auto pinned = parseAndCheckOwnership(
            prog(std::string("using var m := ") + mat + ";"));
        BEZEL_EXPECT_FALSE(pinned.diagnostics.hasErrors());
        BEZEL_EXPECT_FALSE(hasInertRhsWarning(pinned, "m"));
        BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(pinned, "m"));

        auto remedy = parseAndCheckOwnership(
            prog(std::string("const m := ") + mat + ";"));
        BEZEL_EXPECT_TRUE(hasConstMaterializerError(remedy, "m"));
    }
}

// =====================================================================
// (3) FALSE-REJECT GUARD — the narrowness that keeps working code compiling.
// A USER method named `ToArray`/`Sorted` that returns a BORROW of a field is
// NOT an owned production, and §21.1 admits borrows explicitly ("Admitted: […]
// and **borrows** — the binding is then a borrow carrying the initializer's
// source-liveness constraints"). The builtin-only gate (`no resolved
// MethodSymbol`) is what keeps it admissible. A fix keyed on the member NAME
// alone passes (1) and (2) and fails here.
// =====================================================================
BEZEL_TEST(vacuous_lint_materializer_2709,
           user_named_materializer_returning_borrow_admissible_spec_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  private owned _items: Array<int>; "
        "  public constructor() _items := new Array<int>(); _items.Add(7); "
        "    end constructor; "
        "  destructor end destructor; "
        "  public method ToArray(): Array<int> return _items; end ToArray; "
        "  public method Sorted(): Array<int> return _items; end Sorted; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  const t := b.ToArray(); "
        "  const s := b.Sorted(); "
        "  return t.Count + s.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConstMaterializerError(r, "t"));
    BEZEL_EXPECT_FALSE(hasConstMaterializerError(r, "s"));
}

// =====================================================================
// (3b) FALSE-REJECT GUARD, the harder half: a FUNCTION-TYPED FIELD named one
// of the six. `resolveMemberAccess` attaches a `FieldSymbol` — NOT a
// `MethodSymbol` — and the call is typed from the field's `funcReturn`, whose
// plain `Array<string>` slot is declaration-less, so the result-head gate
// passes too. §28.3's producers table classifies a function-value invocation
// with a non-`owned` concrete return as BORROWED, and §21.1 admits borrows,
// so this program is LEGAL.
//
// The first draft of this fix rejected it: its builtin-only gate was a
// `MethodSymbol`-only cast, copied from `isOwningCollectionDrainAliasCall`
// (where the same hole is latent, `Drain` merely being a less plausible field
// name). Verified mechanically — with the cast restored, this exact program
// draws the materializer error at the `const`; with the tightened
// "no symbol at all" gate it compiles clean. The tightening is sound because
// sema's builtin member-call typing path lives entirely inside
// `if (!calleeSym)`, so a genuine builtin never has a symbol attached.
// =====================================================================
BEZEL_TEST(vacuous_lint_materializer_2709,
           function_typed_field_named_materializer_admissible_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cfg "
        "  private owned _items: Array<string>; "
        "  public Split: (Array<string>) -> Array<string>; "
        "  public constructor() "
        "    _items := new Array<string>(); _items.Add(\"x\"); "
        // fix #6630 (§28.3 sink rule, v0.340): the stored lambda neither reads
        // `_items` (a field-stored closure that captures the receiver is
        // rejected at the store) nor mints a fresh array (a fresh owned value
        // at a concrete class-return slot is rejected by §28.3 *Concrete-slot
        // function bodies*). It returns a borrow of its own parameter, which
        // that slot admits. What this row pins is the CALL through a
        // function-typed field named `Split`, which the body does not affect.
        "    Split := lambda(a: Array<string>) => a; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Cfg; "
        "class C public static method Main(): int "
        "  using var cfg := new Cfg(); "
        "  var src := new Array<string>(); src.Add(\"a\"); src.Add(\"b\"); "
        "  const parts := cfg.Split(src); "
        "  return parts.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConstMaterializerError(r, "parts"));
}

// =====================================================================
// (4) OVER-SUPPRESSION GUARD — the lint must still fire where its remedy IS
// legal. A fix that disabled the lint wholesale passes (1)-(3) and fails here.
// The alias and literal cases below are the preserved arms.
// =====================================================================
// #5471 (§29.11, v0.315): the vacuity arm's three-negated check cannot classify
// a lazy query (it is none of a borrow, a disposable class type, or an owning
// choice) and fell through to "value-typed/runtime-managed, no obligation" — a
// default, not a classification. §29.11 names the bound lazy query in K3.14's
// closed suppression set, proving the spec reasons about it in the warning
// context. A bound `Where(…)` is an "ordinary minted caller-owned frame" with a
// real disposal obligation; `using var` over it is the correct pattern and the
// vacuity warning must NOT fire. // per §29.11, §28.3 K3.14 (v0.315)
BEZEL_TEST(vacuous_lint_materializer_2709,
           lazy_query_not_vacuous_alias_and_literal_still_warn_spec_29_11) {
    // A bound lazy query — NOT vacuous: §29.11 frame with real disposal
    // obligation, correctly discharged by `using var`. // per #5471
    auto lazy = parseAndCheckOwnership(
        prog("using var q := a.Where(lambda(x) => x > 0);"));
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(lazy, "q"));

    // A bound-then-pinned collection alias — #2695's preserved arm.
    auto alias = parseAndCheckOwnership(
        prog("var src := new Array<int>(); using var p := src;"));
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(alias, "p"));

    // A runtime-managed literal — the original #2073 shape.
    auto lit = parseAndCheckOwnership(prog("using var s := \"hello\";"));
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(lit, "s"));
}

// §29.11's single-consumption error is the reason the lazy case above must keep
// its steering: re-enumerating a bound lazy query is an ERROR whose own advice
// is `.ToArray()`. That advice now lands on a warning-free `using var`, which is
// the whole point — before this fix, following it produced either the vacuous
// lint (`using var`) or a leak (`const`). Pinned so the affine rule cannot be
// relaxed unnoticed.
//
// Driven through `runAot` rather than `parseAndCheckOwnership`: this diagnostic
// is emitted by a DIFFERENT pass (src/sema/DefiniteAssignment.cpp), which the
// ownership-only harness does not run.
BEZEL_TEST(vacuous_lint_materializer_2709,
           bound_lazy_query_reenumeration_still_rejected_spec_29_11) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  using var q := a.Where(lambda(x) => x > 0); "
        "  var n := 0; "
        "  for var x in q do n := n + 1; end; "
        "  for var y in q do n := n + 1; end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("single-consumption") != std::string::npos &&
            d.message.find("`q`") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// =====================================================================
// (5) LEG 1 — #2146's `Drain()` arm, now reached through the SHARED predicate
// rather than a separately maintained copy on the lint side. Both directions
// must be exactly as before: the const rejection fires (diagnostic text updated:
// fix #4795 makes Drain Owned per stdlib §4.5 v0.307, so the error is the
// "owned production" form, not the v0.195 "deferred-activation" form), and
// the pin stays silent. // per §21.1, §28.3, #4795
// =====================================================================
BEZEL_TEST(vacuous_lint_materializer_2709,
           drain_const_rejection_and_pin_silence_preserved_spec_29_11) {
    const char* kUnit =
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C public static method Main(): int "
        "  using var oa := new OwningArray<Cell>(); "
        "  oa.Add(new Cell()); ";

    auto pinned = parseAndCheckOwnership(
        std::string(kUnit) + "  using var d := oa.Drain(); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(pinned, "d"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(pinned, "d"));

    auto remedy = parseAndCheckOwnership(
        std::string(kUnit) + "  const d := oa.Drain(); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstDrainError(remedy, "d"));
}

// =====================================================================
// (6) THE RUNTIME HALF. The fix must not change what the still-compiling
// spellings DO. `using var` over all six materializers, plus plain `var` over
// the four that bind an `Array`, run to the right value and are neither leaked
// nor double-freed.
//
// Value: the six three-element materializers give 18, `Slice(0..<2)` gives 2
// and `GetBytes("hello")` 5, so the `using var` sweep returns 25 and the
// plain-`var` sweep 12 — 37 per iteration. Asserting the VALUE (not merely
// exit 0) is what makes this non-vacuous: a premature free would corrupt the
// counts, a double free would trip ASan, and a leak would flip `ran`. All
// three failure modes are observable here.
// =====================================================================
BEZEL_TEST(vacuous_lint_materializer_2709,
           materializer_bindings_neither_leak_nor_double_free_spec_28_10) {
    const char* kSrc =
        "module M; class C "
        "  public static method U(a: Array<int>): int "
        "    using var m1 := a.Where(lambda(x) => x > 0).ToArray(); "
        "    using var m2 := a.Where(lambda(x) => x > 0).ToSet(); "
        "    using var m3 := a.Where(lambda(x) => x > 0)"
        ".ToDictionary(lambda(x) => x); "
        "    using var m4 := a.Sorted(); "
        "    using var m5 := a.Reversed(); "
        "    using var m6 := \"a,b,c\".Split(\",\"); "
        "    using var m7 := a.Slice(0..<2); "
        "    using var m8 := Encoding.UTF8.GetBytes(\"hello\"); "
        "    return m1.Count + m2.Count + m3.Count + m4.Count + m5.Count "
        "         + m6.Count + m7.Count + m8.Count; "
        "  end U; "
        "  public static method V(a: Array<int>): int "
        "    var m1 := a.Where(lambda(x) => x > 0).ToArray(); "
        "    var m4 := a.Sorted(); "
        "    var m5 := a.Reversed(); "
        "    var m6 := \"a,b,c\".Split(\",\"); "
        "    return m1.Count + m4.Count + m5.Count + m6.Count; "
        "  end V; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(3); a.Add(1); a.Add(2); "
        "    var s := 0; "
        "    for var i in 0..<5 do s := s + C.U(a) + C.V(a); end; "
        "    return s; "          // 5 * 37 = 185, under runAot's 8-bit mask
        "  end Main; end C;";

    // ASan + LSan: `ran` flips false on a leak AND on a double free.
    auto checked = runAotChecked(kSrc);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 185);

    // The PLAIN (non-sanitized) path must agree — the sanitized build is a
    // different codegen configuration, so a value proven only there is not
    // proven for what ships.
    auto plain = runAot(kSrc);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 185);
}
