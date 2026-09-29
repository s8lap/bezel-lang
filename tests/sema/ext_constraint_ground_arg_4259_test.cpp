// Spec-derived tests for issue #4259 (§26.4 / §14.2 / §17.11, v0.300):
// `ExprTC::extConstraintItemProvablyViolated`'s `new` arm called
// `groundSatisfiesNew` WITHOUT verifying that `arg` satisfies
// `groundSatisfiesNew`'s own precondition — a fully-ground `TypeRef`.
// The issue's own measured `RecP<X>` repro (where `X` is an unresolved outer
// type parameter) compiled cleanly by ACCIDENT: the substituted bare-
// parameter field collides with `typeHasNoDefault`'s own null-declaration
// defer-fallback (`TypeResolution_Checks.cpp:8625`'s `return false;
// // generic param / unresolved / other builtin — defer`). Pre-fix
// `argSatisfiesNew`'s Record arm (TypeResolution_Checks.cpp, lines ~8100-8120)
// was hardened for that population by fix #4166's review follow-up; the
// same hardening never reached `groundSatisfiesNew`'s sibling.
//
// The sound direction for a non-ground `arg` is to DEFER (return `false`,
// KEEP the candidate extension) — the §26.5.1 wording names the lookup
// "an applicability filter, never a ranking signal", and the late
// downstream `where`-clause checks at the tail of `typecheckExpressions()`
// fire later with better messages when they can. Reporting a PROVABLE
// violation for a non-ground `arg` we cannot verify is the wrong direction
// for any kind whose answer depends on `arg`'s `genericArgs` — i.e. the
// Record arm's `seedRecordSubstEnv`/`recordStorageMembers`/`typeHasNoDefault`
// composition (fix #4220).
//
// Row map:
//
//   (1) THE REGRESSION ROW — `extension BoxC<T> where T: new()` applied
//       to a `Choice<X>` receiver (a generic choice whose own genericArgs
//       name the unresolved outer method parameter `X`). Pre-fix the
//       Choice arm at `ExprTypecheck.cpp:1514-1517` returned
//       `decided = true, return false` unconditionally, so `extConstraint
//       ItemProvablyViolated` returned `decided && !ok = true`, wrongly
//       EXCLUDING the extension and emitting "no extension method `Ping`"
//       (§26.5) on `.Ping()`. Post-fix the §26.4 `new` arm defers on a
//       non-ground arg with non-ground genericArgs (the substitution-
//       relevant case) and the extension applies cleanly. This row is
//       where the bug is observable; rows (2)/(3)/(4) are the controls.
//   (2) THE ISSUE'S OWN MEASURED CONTROL — the same extension on
//       `RecP<X>` (a generic record with a bare generic-parameter field).
//       Compiled cleanly pre-fix by ACCIDENT of `typeHasNoDefault`'s
//       defer-fallback (substituted bare-param `T` resolves to `X`,
//       `typeHasNoDefault(X)` falls through to its `return false` defer).
//       Stays cleanly compiled post-fix for the same observable outcome
//       (now explicitly via the guard, not by accident).
//   (3) THE NULL-DECL INTERFACE CONTROL — `extension BoxI<T> where T:
//       new()` applied to a `IDisposable` receiver (a null-declaration
//       builtin interface). `isFullyGroundTypeRef(IDisposable)` is FALSE
//       (per its own header comment, a null-declaration `Named` is treated
//       as non-ground unconditionally), but the §26.4 `new` arm's guard is
//       DELIBERATELY narrower than call site 1's full `isFullyGroundTypeRef`
//       check: `groundSatisfiesNew`'s own Categorical null-decl-interface
//       arm (line 1672, fix #4298) handles `IDisposable` correctly without
//       any generic-arg dependency, and the existing #4298 §26.4 regression
//       row (3) (tests/sema/ground_satisfies_new_null_decl_builtin_4298_
//       test.cpp:210) pins the "no extension method" exclusion. Copy-
//       pasting call site 1's full `isFullyGroundTypeRef` guard here
//       would unground that test (over-accepting instead of excluding).
//   (4) THE GROUND CONTROL — `extension BoxG<T> where T: new()` applied
//       to a `WidgetG` receiver (a plain class with the implicit default
//       constructor). All of `arg->genericArgs` are ground (none), so the
//       guard's loop does not iterate and the predicate reaches
//       `groundSatisfiesNew`, which correctly answers `true` (WidgetG is
//       a class with no explicit constructor — the implicit default
//       satisfies `new()`). Extension applies cleanly — proves the guard
//       does not over-reject on a defaultable-class receiver.
//
// MECHANICAL FAIL-WITHOUT / PASS-WITH: see the commit body for the exact
// "N of M" count and row names.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::parseAndTypecheckGated;

bool hasDiagnosticContaining(const bezel::test::SemaResult& r,
                              std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

// (1) THE REGRESSION ROW — generic method calling `.Ping()` on
// `BoxC<Choice4259<X>>`, where `Choice4259<X>` is a generic choice whose
// own genericArgs name the unresolved outer method parameter `X`. The
// extension's `where T: new()` filter binds `T := Choice4259<X>`. Pre-fix
// the §26.4 `new` arm's call to `groundSatisfiesNew(Choice4259<X>, ...)`
// reached the Choice arm (line 1514-1517) which returns
// `decided = true, return false` unconditionally — reported as a PROVABLE
// violation for a non-ground arg we cannot verify, wrongly EXCLUDING the
// candidate extension. The same `groundSatisfiesNew` is called at the
// §19.6 exhaustiveness leaf filter too (line 1780), but that call site is
// gated on `isFullyGroundTypeRef(arg)` and never reaches the Choice arm
// for this receiver. Post-fix the §26.4 `new` arm defers (returns `false`,
// keeps the candidate) on a non-ground arg with non-ground genericArgs;
// the extension applies cleanly and `.Ping()` resolves.
//
// FAILS WITHOUT THE FIX: `BoxC<Choice4259<X>>.Ping()` is rejected with
// "no extension method `Ping` on type `BoxC` takes 0 argument(s)
// (§26.5)" — the same wording `tests/sema/ground_satisfies_new_null_decl
// _builtin_4298_test.cpp` row (3) asserts on `Box4298d<IDisposable>`.
// PAINTS-WITH: compiles cleanly, no diagnostic.
BEZEL_TEST(sema_ext_constraint_ground_arg_4259,
           choice_with_unresolved_param_keeps_extension_spec_26_4) {
    auto r = bezel::test::parseAndCollect(
        "module M4259a; "
        "class BoxC4259<T> end BoxC4259; "
        "public choice Choice4259<X> "
        "  None(i: int), "
        "  Just(v: X); "
        "end; "
        "extension BoxC4259<T> where T: new() "
        "  public method Ping(): int => 1; "
        "end; "
        "class Caller4259<X> "
        "  public static method M(b: BoxC4259<Choice4259<X>>): int "
        "    return b.Ping(); "
        "  end M; "
        "end Caller4259; "
        "class Entry4259a "
        "  public static method Main(): int "
        "    var b := new BoxC4259<Choice4259<int>>(); "
        "    return Caller4259<int>.M(b); "
        "  end Main; "
        "end Entry4259a;");
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (2) THE ISSUE'S OWN MEASURED CONTROL — generic method calling
// `.Ping()` on `BoxP<RecP4259<X>>`, where `RecP4259<X>` is a generic
// record (its only field `V: T` uses the record's own generic parameter).
// Pre-fix compiled cleanly by accident of `typeHasNoDefault`'s defer-
// fallback (the substituted bare-param `T` resolves to `X`, and
// `typeHasNoDefault(X)` falls through to its `return false` defer at
// `TypeResolution_Checks.cpp:8625`). Post-fix the §26.4 `new` arm's
// narrower guard defers on the same shape (X is non-ground, the
// genericArgs loop sees it), reaching the same observable outcome.
// Compiles cleanly both pre-fix and post-fix; pins that the fix is not a
// behavioral change for the issue's own measured repro.
//
// MEASURED identical on both sides of the fix (this lane's own tree,
// `--area sema 4259`): compiles clean pre-fix and post-fix — not a
// FAIL-WITHOUT/PASS-WITH row, a same-on-both-sides boundary pin
// (LANE_RULES §20: state a control's reason explicitly rather than let a
// same-on-both-sides row read as an oversight). Both rows (1) and (2)
// are required: row (1) demonstrates the bug exists in a measurable
// shape; row (2) proves the fix does not regress the issue's own measured
// repro.
BEZEL_TEST(sema_ext_constraint_ground_arg_4259,
           record_with_unresolved_param_keeps_extension_spec_26_4) {
    auto r = bezel::test::parseAndCollect(
        "module M4259b; "
        "class BoxP4259<T> end BoxP4259; "
        "public record RecP4259<T>(V: T); "
        "extension BoxP4259<T> where T: new() "
        "  public method Ping(): int => 1; "
        "end; "
        "class Caller4259b<X> "
        "  public static method M(b: BoxP4259<RecP4259<X>>): int "
        "    return b.Ping(); "
        "  end M; "
        "end Caller4259b; "
        "class Entry4259b "
        "  public static method Main(): int "
        "    var b := new BoxP4259<RecP4259<int>>(); "
        "    return Caller4259b<int>.M(b); "
        "  end Main; "
        "end Entry4259b;");
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (3) THE NULL-DECL INTERFACE CONTROL — `extension BoxI4259<T> where T:
// new()` applied to a `IDisposable` receiver. `isFullyGroundTypeRef(
// IDisposable)` is FALSE (per its own header comment, a null-declaration
// `Named` is treated as non-ground unconditionally), but the §26.4 `new`
// arm's guard is DELIBERATELY narrower than call site 1's full
// `isFullyGroundTypeRef` check: it iterates ONLY `arg->genericArgs` (none,
// for a bare `IDisposable` argument), so the predicate reaches
// `groundSatisfiesNew` and the Categorical null-decl-interface arm at
// line 1672 (fix #4298) returns `decided = true, return false`. The
// extension is correctly EXCLUDED and `Make()` emits the same
// `no extension method` (§26.5) diagnostic the sibling #4298 test row
// (3) asserts on `Box4298d<IDisposable>`.
//
// Compiles-with-error both pre-fix and post-fix; pins that the narrower
// guard does not regress the #4298 regression. LANE_RULES §20 again:
// state the reason for the same-on-both-sides row rather than let it
// read as an oversight — the explicit "DO NOT copy call site 1's full
// `isFullyGroundTypeRef` guard here" comment at the call site is the
// load-bearing reason this row stays identical on both sides.
BEZEL_TEST(sema_ext_constraint_ground_arg_4259,
           null_decl_interface_still_excludes_extension_spec_26_4) {
    auto r = bezel::test::parseAndCollect(
        "module M4259c; "
        "class BoxI4259<T> end BoxI4259; "
        "extension BoxI4259<T> where T: new() "
        "  public method Ping(): int => 1; "
        "end; "
        "class Entry4259c "
        "  public static method Main(): int "
        "    var b := new BoxI4259<IDisposable>(); "
        "    return b.Ping(); "
        "  end Main; "
        "end Entry4259c;");
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasDiagnosticContaining(r, "no extension method `Ping`"));
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, "\xC2\xA7""26.5"));
}

// (4) THE GROUND CONTROL — `extension BoxG4259<T> where T: new()`
// applied to a `WidgetG4259` receiver (a plain class with the implicit
// default constructor). All of `arg->genericArgs` is empty, so the
// §26.4 `new` arm's guard's loop does not iterate; the predicate reaches
// `groundSatisfiesNew`, which correctly answers `true` via the Class arm
// (line 1589-1614: `ctors.empty()` returns `true` for the implicit
// default). Extension applies cleanly. Proves the guard does not over-
// reject on a defaultable-class receiver.
//
// Compiles cleanly both pre-fix and post-fix; pins the negative direction
// of the regression — the fix must not introduce a regression on the
// defaultable case.
BEZEL_TEST(sema_ext_constraint_ground_arg_4259,
           defaultable_class_receiver_keeps_extension_spec_26_4) {
    auto r = bezel::test::parseAndCollect(
        "module M4259d; "
        "class BoxG4259<T> end BoxG4259; "
        "extension BoxG4259<T> where T: new() "
        "  public method Ping(): int => 1; "
        "end; "
        "class WidgetG4259 end WidgetG4259; "
        "class Entry4259d "
        "  public static method Main(): int "
        "    var b := new BoxG4259<WidgetG4259>(); "
        "    return b.Ping(); "
        "  end Main; "
        "end Entry4259d;");
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
