// fix #6347 / spec §5.1 "Reference conversions" (v0.338) — a GENERIC built-in
// class (`Array<T>`, `LinkedList<T>`, `Set<T>`, …) is implicitly convertible to
// an UNRELATED USER class.
//
// #4733 (`check_assignable_builtin_named_hole_4733_test.cpp`) closed exactly
// this hole for a NON-generic built-in: `var w: Widget := new
// StringBuilder();` is rejected by the "Repro A" arm in `ExprTC::
// checkAssignable`, whose predicate `fromBuiltinClass` is
//
//     from->kind == Named && !from->declaration && from->genericArgs.empty()
//         && !fromIsTypeVar && isBuiltinClassHead(fromName)
//
// The `from->genericArgs.empty()` conjunct is DELIBERATE — the sibling test's
// own header says so: it exists so that a built-in→built-in VARIANCE pair
// (`Array<T>` → `IEnumerable<T>`) falls through to the §12.9 variance arm
// rather than being caught by the Repro-A gate. But that arm is keyed on a
// NULL-DECLARATION TARGET:
//
//     if (from->kind == Named && to->kind == Named && !to->genericArgs.empty()
//             && !to->declaration) { … }
//
// A USER class target HAS a declaration, so the variance arm never sees the
// pair — and `genericArgs.empty()` had already taken the source out of the
// Repro-A gate. The fourth quadrant of the grid (generic built-in source ×
// user-class target) therefore fell through to the catch-all accept:
//
//     using var a := new Array<int>();
//     a.Add(1); a.Add(2); a.Add(3);
//     var b: Box<int> := a;   // COMPILES AND RUNS
//     Console.WriteLine(b.V); // 0 — `Box<int>`'s field V read out of
//                              //   the ARRAY's first word (a count)
//
// That is a silent miscompile, not a cosmetic acceptance: `b` is the same
// reference as `a`, re-typed, and every field read through `b` reads the
// array's representation. MEASURED on the pre-fix binary: `Array<int>`,
// `LinkedList<int>` and `Set<int>` all ACCEPT at a variable initialiser, a
// call argument and a return alike.
//
// §5.1's reference-conversion list is CLOSED — "The following conversions are
// implicit" enumerates identity, class-to-base-transitively, anything-to-
// `Object`, type-to-an-interface-it-conforms-to, interface-to-extended-
// interface, type-parameter-to-its-bound, and §12.9 variance. An unrelated
// user class is none of them, and §5.1 defines a reference conversion as
// "representation-preserving — the same reference viewed at a WIDER static
// type"; `Box` is not wider than `Array<int>`, it is a DIFFERENT type.
//
// The fix introduces `fromBuiltinClassAnyArgs` — the same predicate with the
// `genericArgs.empty()` conjunct dropped — and uses it for the two
// `toUserClass` arms only. Every other consumer of `fromBuiltinClass` keeps
// the original restriction, so the built-in→built-in variance path the
// restriction was written for is untouched.
//
// Rows below:
//
//   1–3. THE BUG: `Array<int>` → unrelated `Box`, at all three positions the
//        issue measured (initialiser, call argument, return). Pre-fix:
//        ACCEPT and miscompiled — the return-position repro does not merely
//        read a wrong field, it RUNS AWAY (the harness reports a 30s
//        timeout, which is explicitly NOT a compiler rejection).
//   4–5. `LinkedList<int>`, `Set<int>` — the issue's other named kinds.
//   6.   `Dictionary<K,V>` — a generic built-in of ARITY 2, proving the
//        predicate is not one-argument special.
//   7.   Nested generic — `Array<Array<int>>` (the issue's row 4).
//   8.   THE OWNINGARRAY CONTRAST, filed explicitly — see its own header.
//   9.   THE SECOND BEHAVIOR FLIP — `Array<int>` → `GBox<Array<int>>`,
//        which #6347 called "correct — §14.5 identity". See its header.
//   9b.  CONTROL — §14.5 inference through a BARE type parameter, the
//        correct spelling of row 9.
//  10.   CONTROL — the variance case #4733's ROW 3 pins, at a different
//        arity so identity cannot satisfy it: `Array<int>` →
//        `IEnumerable<int>` stays ACCEPTED.
//  11.   CONTROL — a user class upcast, the other §5.1 bullet: `Derived` →
//        `Base` stays ACCEPTED.
//  12.   CONTROL — `Object`: §5.1's "any type to `Object`" must still admit
//        `Array<int>`, including through the `pendingTypeFactAdmits`
//        escape-hatch row that shares the widened predicate.
//
// MEASURED fail-without (re-measured by reviewer r31-rv-6347, who asserted
// distinct test-binary md5s on each leg): reverting only
// `src/sema/ExprTypecheck.cpp` to HEAD and REBUILDING THE TEST BINARY takes
// this file from `14 passed, 0 failed` to `4 passed, 10 failed`.
//
// The ten rows that flip are 1–7 (repro), 9 (the generic-target flip) and
// BOTH OwningArray rows: 6a fails pre-fix only on the
// `hasBuiltinToUserClassDiagnostic` assertion (the #1327 diagnostic IS
// present pre-fix, which is that row's point), and 6b fails all three
// because `OwningArray<Box>` is ACCEPTED pre-fix.
//
// The four controls green on BOTH trees are `variance`, `bare_type_parameter`,
// `user_class_upcast` and `object`.
//
// Diagnostic wording is pinned by SUBSTRING on the rule's identifying tokens
// (the built-in name, the target name, "not a base of it", "§5.1"), matching
// the convention the sibling #4733 file established.

#include <string>

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §5.1 fragment for the Repro-A arm: "the built-in type `Array` is not a
// `Box` — `Box` is not a base of it, so the conversion would be a narrowing
// (or unrelated) reference conversion, and §5.1 has none implicitly".
// The fragment pins the RULE (§5.1) and its identifying tokens, so a
// rephrase that keeps them does not silently break the pin.
bool hasBuiltinToUserClassDiagnostic(const JitResult& r,
                                     std::string_view fromName,
                                     std::string_view toName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(std::string("the built-in ") ) == std::string::npos)
            continue;
        if (d.message.find(std::string(fromName)) == std::string::npos) continue;
        if (d.message.find(std::string("is not a `") + std::string(toName) +
                           "`") == std::string::npos)
            continue;
        if (d.message.find("not a base of it") == std::string::npos) continue;
        if (d.message.find("5.1") == std::string::npos) continue;
        return true;
    }
    return false;
}

// Shared prelude. `Box` is an UNRELATED user class — no declared base, no
// interface, no shared ancestor with any built-in. Every program declares a
// `Main()` so `runAot` can fork a complete compilation.
const std::string kPrelude =
    "module M; import System.Text; import System.Collections.Generic; "
    "class Box "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Box; ";

}  // namespace

// =====================================================================
// ROW 1 — THE BUG: `Array<int>` → unrelated `Box`, at all three
// positions the issue measured (initialiser, call argument, return).
// Pre-fix: ACCEPTED and the miscompiled program runs.
// Post-fix: rejected with §5.1 "is not a `Box`" + "not a base of it".
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           array_int_to_unrelated_box_initializer_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Take(b: Box): int "
        "    return b.V; "
        "  end Take; "
        "  public static method Make(s: Array<int>): owned Box "
        "    return new Box(1); "
        "  end Make; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    var b: Box := a; "          // the defect: generic built-in → user class
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinToUserClassDiagnostic(r, "Array", "Box"));
}

BEZEL_TEST(builtin_generic_to_user_class_6347,
           array_int_to_unrelated_box_argument_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Take(b: Box): int "
        "    return b.V; "
        "  end Take; "
        "  public static method Make(s: Array<int>): owned Box "
        "    return new Box(1); "
        "  end Make; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    var n := Take(a); "         // the defect, at an ARGUMENT position
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinToUserClassDiagnostic(r, "Array", "Box"));
}

BEZEL_TEST(builtin_generic_to_user_class_6347,
           array_int_to_unrelated_box_return_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Make(s: owned Array<int>): owned Box "
        "    return s; "               // the defect, at a RETURN position
        "  end Make; "
        "  public static method Main(): int "
        "    using var b := Make(new Array<int>()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinToUserClassDiagnostic(r, "Array", "Box"));
}

// =====================================================================
// ROW 2 — `LinkedList<int>`, the issue's second named kind.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           linkedlist_int_to_unrelated_box_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new LinkedList<int>(); "
        "    var b: Box := a; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinToUserClassDiagnostic(r, "LinkedList", "Box"));
}

// =====================================================================
// ROW 3 — `Set<int>`, the issue's third named kind.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           set_int_to_unrelated_box_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Set<int>(); "
        "    var b: Box := a; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinToUserClassDiagnostic(r, "Set", "Box"));
}

// =====================================================================
// ROW 4 — `Dictionary<K,V>`: a generic built-in with an ARITY > 1, to
// prove the widened predicate is not a one-argument special case.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           dictionary_kv_to_unrelated_box_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int, string>(); "
        "    var b: Box := d; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinToUserClassDiagnostic(r, "Dictionary", "Box"));
}

// =====================================================================
// ROW 5 — nested generic: `Array<Array<int>>`. The source's own argument
// is itself a built-in collection, which must not perturb the head test.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           nested_array_to_unrelated_box_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<Array<int>>(); "
        "    var b: Box := a; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinToUserClassDiagnostic(r, "Array", "Box"));
}

// 6a pins the artifact AND its true consequence: the `int` cell IS rejected
//     by stdlib §4.2 / #1327, but the sema pass does NOT stop there — it
//     continues into the conversion check. MEASURED: pre-fix the program
//     reports ONLY the #1327 diagnostic; post-fix this arm's diagnostic
//     appears alongside it. So #1327 was never what protected
//     `OwningArray<T>` from this rule — it merely fired first.
// 6b pins the truth: give `OwningArray` the class element type #1327
//     requires, and it converts to the unrelated `Box` exactly as `Array`
//     does — MEASURED ACCEPTED on the pre-fix binary, same as `Array<Box>`.
//     So `OwningArray` was never a "correctly rejected" control, and the
//     issue's proposed discriminator rests on a type that cannot be written.
// =====================================================================

// stdlib §4.2 / §14.4 (#1327): "`OwningArray` requires a class or interface
// element type". Substring-pinned on the rule's own identifying token.
bool hasOwningElementTypeDiagnostic(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("requires a class or interface element type") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

BEZEL_TEST(builtin_generic_to_user_class_6347,
           owningarray_of_primitive_also_reaches_this_arm_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new OwningArray<int>(); "  // illegal element type
        "    var b: Box := a; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The pre-existing #1327 reason is still reported …
    BEZEL_EXPECT_TRUE(hasOwningElementTypeDiagnostic(r));
    // … and so is this arm's, which was absent pre-fix. If #1327 ever stops
    // firing, this row still holds the conversion rule in place.
    BEZEL_EXPECT_TRUE(
        hasBuiltinToUserClassDiagnostic(r, "OwningArray", "Box"));
}

BEZEL_TEST(builtin_generic_to_user_class_6347,
           owningarray_of_class_to_unrelated_box_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new OwningArray<Box>(); "  // legal #1327 element
        "    var b: Box := a; "                        // ACCEPTED pre-fix
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasBuiltinToUserClassDiagnostic(r, "OwningArray", "Box"));
}

// =====================================================================
// ROW 7 — CONTROL: the variance case the `genericArgs.empty()` conjunct
// was written for. `Array<int>` → `IEnumerable<int>` is §12.9's variance
// arm and MUST stay accepted. #4733's ROW 3 pins the same shape; this row
// exists because that conjunct is the one thing this fix relaxes, so the
// case it protects needs an independent pin in the file that relaxes it.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           array_to_ienumerable_variance_still_accepted_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    var e: IEnumerable<int> := a; "   // built-in → built-in interface
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// ROW 8 — THE SECOND BEHAVIOR FLIP, filed explicitly. #6347's matrix
// recorded `Array<int>` → `Box<Array<int>>` as "accepted (this one is
// correct — §14.5 identity)". That reading is wrong, and this row is the
// one place the fix removes an acceptance the ISSUE called correct.
//
// `GBox<Array<int>>` is NOT the same type as `Array<int>`, so §5.1's
// identity row — "every type converts to itself" — does not reach it, and
// no other §5.1 reference conversion does either. §14.5 is type-argument
// INFERENCE, not a conversion: it says a bare `X` "reached through a
// constructed parameter type" binds to the argument's static type, after
// which §5.1 governs the argument check itself. Bezel has no implicit
// conversion operators (rationale §14.5, "No implicit conversion
// operators"), so there is no wrapping path from `Array<int>` into
// `GBox<Array<int>>` either.
//
// MEASURED ACCEPTED on the pre-fix binary — the same §5.1 hole, reached
// through a GENERIC user-class target rather than a plain one. `toUserClass`
// keys on `to->declaration` and never examined `to->genericArgs`, so a
// generic target slipped through exactly as a plain one did.
//
// The write is `new GBox<Array<int>>()`, or a `Wrap<T>(x: T)` helper
// (ROW 8b), which is the §14.5-correct spelling.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           array_int_to_generic_user_box_rejected_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "class GBox<T> "
        "  public V: T?; "
        "end GBox; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    var b: GBox<Array<int>> := a; "  // MEASURED ACCEPTED pre-fix
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBuiltinToUserClassDiagnostic(r, "Array", "GBox"));
}

// =====================================================================
// ROW 8b — CONTROL: §14.5 inference through a BARE type parameter, which
// IS the correct spelling of ROW 8 and must keep working. `Wrap<T>(x: T)`
// binds `T := Array<int>` at a bare `X` position (§14.5 phase 1, first
// bullet) and never asks §5.1 to convert the collection into the box, so
// the widened source predicate must not touch it.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           bare_type_parameter_inference_still_accepted_spec_14_5) {
    JitResult r = runAot(kPrelude +
        "class GBox<T> "
        "  public V: T?; "
        "end GBox; "
        "public class Program "
        "  public static method Wrap<T>(x: T): owned GBox<T> "
        "    return new GBox<T>(); "
        "  end Wrap; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    using var b := Wrap(a); "     // T binds to Array<int>
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// ROW 9 — CONTROL: a USER class upcast, §5.1's "class to base class,
// transitively". The widened source predicate must not confuse a real
// declared-base walk for a violation.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           user_class_upcast_still_accepted_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "class Base "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Base; "
        "class Derived : Base "
        "  public constructor(v: int) base(v); end constructor; "
        "end Derived; "
        "public class Program "
        "  public static method Main(): int "
        "    using var d := new Derived(5); "
        "    var b: Base := d; "           // §5.1 upcast
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// ROW 10 — CONTROL: §5.1's "any type to `Object`". A built-in generic
// still converts to `Object`, and it does so through the OTHER arm that
// shares the widened predicate — the `pendingTypeFactAdmits` escape row
// sits directly above the rejecting row, so it is the row a careless
// widening of the rejecting arm alone would silently change.
// =====================================================================

BEZEL_TEST(builtin_generic_to_user_class_6347,
           array_int_to_object_still_accepted_spec_5_1) {
    JitResult r = runAot(kPrelude +
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    var o: Object := a; "          // §5.1 any-type-to-Object
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}