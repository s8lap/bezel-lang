// Codegen/sema — issue #2480: `a.CompareTo(b)` on a bare-type-parameter `T`
// value receiver silently returned `0` for EVERY argument pair when the
// concrete substitution did not actually implement `IComparable<T>` —
// reproduces identically for a record and a choice substitution, so it is
// not substitution-kind-specific.
//
// Root cause (two cooperating gaps, both fixed by this commit):
//
//   1. `ExprTC::typeCall`'s #1078 "unconstrained type parameter" gate
//      (src/sema/ExprTypecheck_Calls.cpp, ~line 2852) hardcoded `CompareTo`
//      into its `isObjectMember` set alongside the FOUR real §8.1 Object
//      members (`ToString`/`GetHashCode`/`Equals`/`GetType` — guaranteed on
//      every type unconditionally). But `CompareTo` is not one of them:
//      §8.1's Object surface is exactly `GetType`/`Equals`/`GetHashCode`/
//      `ToString`. `CompareTo` comes from `IComparable<T>` conformance
//      (stdlib §5, §14.2's "where T: IComparable<int>" worked example),
//      which `where T: value` alone does not establish (§14.2's `value`
//      row: "records, enums, choices, primitives, inline arrays ... the
//      unit type" — nothing about ordering).
//
//   2. `ExprTC::resolveMemberAccess`'s "issue #605" bare-type-parameter
//      block (~line 12282) had the identical unconditional typing for
//      `CompareTo` (mirroring the true-universal `Equals`), with a stale
//      comment claiming the builtin `IComparable<T>` "has a null TypeDecl,
//      so the constraint-member resolution above skips it" — no longer
//      true since #1115 gave `IComparable<T>` a real synthesized Interface
//      TypeSymbol precisely so the #484 constraint-member walk (~line
//      10326) CAN find its `CompareTo` member through a genuine `where T:
//      IComparable<T>` bound (verified below: with such a bound present,
//      #484 resolves the call through the ordinary member-call path and
//      this fallback is never reached at all).
//
// Both gaps let sema type the call as valid `int` for ANY declared bare-T
// receiver regardless of whether its constraints actually guarantee
// ordering, so codegen's #903 resolver (LlvmBackend_Calls2.cpp) — which
// only binds `calleeSym` when the concrete substitution's declaration kind
// is Record/Class AND that type has a REAL declared `CompareTo` — fell
// through to an unrelated stub emitting a constant `0` for every other
// substitution shape, silently, with no diagnostic.
//
// The fix: both sites now re-run the same `forEachConstraintBound` walk
// #484 already performs, admitting `CompareTo` only when T's constraints
// actually provide it. When absent, the call is a compile-time reject —
// "no member `CompareTo` on unconstrained type parameter `T`; add a `where
// T: <constraint>` that provides this member (§14.2)" — the SAME
// diagnostic #1078 already uses for any other unconstrained-member call, so
// this is not a new diagnostic shape, just a corrected admission set.
//
// A THIRD gap, found while writing this fix's own adversarial regression
// grid (a *legitimately* `where T: IComparable<T>`-constrained bare-T,
// substituted with a CHOICE that DOES implement `IComparable<Color>`): the
// sema-side fix alone was not sufficient for choice — codegen's two
// value-type devirtualization gates in `Emitter::lowerCall`
// (LlvmBackend_Calls2.cpp) each special-cased Record/Class (the `!calleeSym`
// #903 gate, ~line 4105) or Record/Enum (the `ms->parent->kind==Interface`
// gate #1495 added Enum to, ~line 6116) but never Choice, so a choice
// substitution fell through to itable dispatch on a non-pointer receiver —
// the exact #1495 "receiver-against-itself" shape that used to miscompile
// enums, silently yielding `0`. Fixed by adding `ast::TypeDeclKind::Choice`
// to both gates: choices are sealed (§35.1) and statically dispatched
// (§35.2) exactly like records/enums, so devirtualizing directly to the
// choice's own declared method is sound on the identical reasoning #1495
// already established. See `choice_with_icomparable_constraint_still_
// dispatches` below — value-correct (7, not 0), confirmed silently wrong
// before this commit's codegen fix even with sema's own gap already closed.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

const char* kExpectedDiag =
    "no member `CompareTo` on unconstrained type parameter `T`";

}  // namespace

// ── The exact #2480 repros: now a clean compile-time reject ─────────────

// #2480's own record repro: `Pt` implements no `IComparable<Pt>` at all.
// Before the fix this compiled clean and printed `0` for every distinct
// pair (confirmed both argument orders); the correct behavior is a
// compile-time reject, since nothing in `where T: value` requires or
// implies ordering (§14.2).
BEZEL_TEST(generic_bare_typeparam_compareto_reject_2480,
           record_without_icomparable_rejected) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method F<T>(a: T, b: T): int where T: value "
        "    return a.CompareTo(b); "
        "  end F; "
        "  public static method Main(): int "
        "    var c1 := F(new Pt(1, 1), new Pt(9, 9)); "
        "    return c1; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(program, kExpectedDiag));
}

// #2480's choice repro — the shape that motivated discovering the bug while
// sweeping #2476's fallout (choice was untested against `where T: value`
// before that fix admitted it), but confirmed pre-existing and unrelated to
// #2476's own scope: a record reproduces identically (test above).
BEZEL_TEST(generic_bare_typeparam_compareto_reject_2480,
           choice_without_icomparable_rejected) {
    const std::string program =
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "class C "
        "  public static method F<T>(val: T): int where T: value "
        "    return val.CompareTo(val); "
        "  end F; "
        "  public static method Main(): int "
        "    return F(Color.Red); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(program, kExpectedDiag));
}

// A PRIMITIVE substitution (int) is rejected too, even though `int` itself
// DOES conform to `IComparable<int>` at the concrete-type level (§14.2 rule
// 5) — the generic BODY cannot rely on that, because `where T: value` names
// no such bound, and per §14.2's own account of `where T: IFoo`, "[t]he
// body cannot tell whether T was satisfied by a class, a record, an enum,
// an interface, a primitive, or another type parameter" UNLESS the
// constraint itself says so. Confirms the fix is not merely "does the
// substitution happen to work" but the correct generics discipline (the
// same one #1078 already applies to every other unconstrained member).
BEZEL_TEST(generic_bare_typeparam_compareto_reject_2480,
           primitive_without_icomparable_constraint_rejected) {
    const std::string program =
        "module M; "
        "class C "
        "  public static method F<T>(a: T, b: T): int where T: value "
        "    return a.CompareTo(b); "
        "  end F; "
        "  public static method Main(): int "
        "    return F(1, 9); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(program, kExpectedDiag));
}

// ── Regression guards: the LEGITIMATE `where T: IComparable<T>` shapes ──

// A record that DOES implement `IComparable<Pt>` through a genuine `where
// T: IComparable<T>` constraint dispatches to its REAL `CompareTo` — VALUE-
// correct, not just clean compile: Pt(1,1).CompareTo(Pt(9,9)) = 1-9 = -8.
// This is the #484 constraint-member path this fix's new check defers to;
// pinning it here guards against the fix accidentally narrowing it.
BEZEL_TEST(generic_bare_typeparam_compareto_reject_2480,
           record_with_icomparable_constraint_still_dispatches) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int): IComparable<Pt> "
        "  public method CompareTo(other: Pt): int return X - other.X; end CompareTo; "
        "end Pt; "
        "class C "
        "  public static method F<T>(a: T, b: T): int where T: IComparable<T> "
        "    return a.CompareTo(b); "
        "  end F; "
        "  public static method Main(): int "
        "    return F(new Pt(1, 1), new Pt(9, 9)); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), static_cast<int>(
        static_cast<unsigned char>(-8)));
}

// A choice that DOES implement `IComparable<Color>` also dispatches
// correctly through the constraint — proves the fix does not collapse
// "constrained" and "unconstrained" together for the choice shape either
// (mirrors #2476's own concern that choice-specific fallout be checked).
//
// The CompareTo body deliberately pattern-matches on `other` rather than
// comparing `this = other` — comparing `this` against a same-typed by-value
// parameter with `=` inside a choice (or record) INSTANCE method was found,
// while writing this test, to crash the compiler unconditionally (an LLVM
// `ICmpInst::AssertOK` operand-type-mismatch abort, reproducing identically
// for a non-generic direct call with no `where`/no `IComparable` involved
// at all — `this` inside a value-type instance method is a borrow per
// §35.2/§9.2, apparently lowered to a different LLVM representation than an
// ordinary by-value same-typed parameter at an `=` site). That is a
// separate, pre-existing bug, filed as #2497; not exercised here to avoid
// encoding a known-separate crash as expected behavior in this fix's own
// suite (the same discipline #2476's own suite already follows for #2480
// itself, and #2465's for #2469).
BEZEL_TEST(generic_bare_typeparam_compareto_reject_2480,
           choice_with_icomparable_constraint_still_dispatches) {
    const std::string program =
        "module M; "
        "choice Color: IComparable<Color> "
        "  Red, Green(value: int), Blue; "
        "  public method CompareTo(other: Color): int "
        "    return (case other of "
        "      Red => 10, "
        "      Green(v) => v, "
        "      Blue => 20); "
        "  end CompareTo; "
        "end Color; "
        "class C "
        "  public static method F<T>(val: T, other: T): int where T: IComparable<T> "
        "    return val.CompareTo(other); "
        "  end F; "
        "  public static method Main(): int "
        "    return F(Color.Red, Color.Green(7)); "  // dispatches to the real method: 7
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 7);
}

// Combining `value` with the interface bound (`where T: value,
// IComparable<T>`) — a legal combined constraint per §14.2 ("Constraints
// may be combined") — must still find and dispatch the real member, not
// be confused by the extra `value` item alongside the `type`-kind
// IComparable<T> item in the same `where` clause.
BEZEL_TEST(generic_bare_typeparam_compareto_reject_2480,
           combined_value_and_icomparable_constraint_dispatches) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int): IComparable<Pt> "
        "  public method CompareTo(other: Pt): int return X - other.X; end CompareTo; "
        "end Pt; "
        "class C "
        "  public static method F<T>(a: T, b: T): int where T: value, IComparable<T> "
        "    return a.CompareTo(b); "
        "  end F; "
        "  public static method Main(): int "
        "    return (if F(new Pt(9, 0), new Pt(1, 0)) > 0 then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}
