// Codegen — spec §12.7 / §14 / §8.6, v0.307 — issue #4926 (narrow): a builtin
// GENERIC COLLECTION type-test target (Array<T>, LinkedList<T>, ...) has no
// runtime descriptor at all — these are sealed, non-boxed reference types
// (§8.6 only boxes a VALUE type reaching an Object/interface location), so
// `lowerTypeTest` fell straight through to its "no runtime descriptor
// resolved" bail and silently answered `false` for EVERY `is` / `is not` /
// `as Array<T>` test — matched or not — with NO diagnostic at all (#3079).
//
// This narrow fix decides the STATICALLY DECIDABLE subset with zero runtime
// cost, exploiting that these collection classes are SEALED (no
// subclassing) and Bezel generics are INVARIANT (§14, no covariance):
//   Case A — the operand's static type is itself a ground instance of a
//            builtin collection (no Object/interface erasure in the way):
//            same head + identical generic arguments -> true; a different
//            head, or a mismatched argument, -> false.
//   Case B — the operand is erased to a single-element-type-parameter
//            builtin interface whose OWN argument is statically known and
//            differs from the target's -> false (invariance rules out every
//            possible concrete backing at once, with no runtime check).
//
// ⚠ Case B is restricted to `ICollection<T>`, `IList<T>`,
// `IOwningCollection<T>`, `IOwningList<T>` and `IOwningEnumerable<T>` — the
// five interfaces stdlib declares INVARIANT — since #5077 (a reviewer-caught
// unsoundness in the original #4926 landing, `5f2c8753`). `IEnumerable<out
// T>`, `IReadOnlyCollection<out T>` and `IReadOnlyList<out T>` are
// stdlib-COVARIANT (§12.9), so a mismatched argument on one of THOSE proves
// nothing (§12.9's own worked example: `IEnumerable<Dog>` is assignable to
// `IEnumerable<Animal>`, an argument "mismatch" that is actually a true
// subtyping relation) — folding them into Case B answered a confident,
// undiagnosed `false` for `v: IEnumerable<Animal> := (an Array<Dog>); v is
// Array<Dog>`, which is WRONG (must be `true`) and worse than the pre-#4926
// state because it pre-empted the #2775 warning. See
// tests/lit/type_test_covariant_interface_carrier_warns_5077.bzl for the
// three covariant repros (pinned via the #2775 warning, not a gtest value,
// since none has a trivially sound decision) and the
// `interface_carrier_invariant_ilist_mismatched_element_type_...` row below
// for Case B's still-decisive invariant path.
//
// UPDATE — fix #6776 (§8.2 / §12.7, v0.340): the positive runtime tag the
// next paragraph says "does not exist yet" now exists (word 0 of a bz_array_t
// is the runtime-owned `bz_array_vtable`, plus a per-instantiation descriptor
// codegen stamps at construction; see bezel_runtime_refcounting.cpp), so the
// OBJECT carrier is decided: `object_carrier_decided_by_runtime_tag_spec_12_7_6776`
// below answers `true`. The matching-element INTERFACE carrier is still
// `false` through the #2775 bail, for a different reason than the tag: an
// interface-typed location holds the caller-materialised adapter object, not
// the array itself, so its word 0 is not the array's tag. The paragraph below
// describes the pre-#6776 state and is kept as the record of it.
//
// NOT covered here (still genuinely ambiguous, UNCHANGED behavior): an
// Object carrier, or a MATCHING-element-type interface carrier — exactly the
// issue's own headline repro (`e: IEnumerable<char> := a; e is Array<char>`;
// `o: Object := a; o is Array<char>`). Deciding those needs a positive
// runtime "this is an Array, not a bare string" tag that does not exist yet:
// bz_obj_is_string (narrow #4858) is a NEGATION heuristic that cannot tell a
// string from an array/list/dict within its vtable-less bucket — #4885's own
// defect. That gap is filed as #5059. Those rows are pinned as UNCHANGED
// behavior (`false`, no crash) below rather than left unasserted; this fix
// DOES additionally make that fallthrough correctly emit the "internal: this
// type test was DROPPED" diagnostic (#2775) instead of complete silence —
// `concreteTarget`'s classification excluded EVERY declaration-less Named
// target pre-fix, conflating the genuinely-dead bare-type-parameter shape
// with this reachable, executed one.
//
// #4909 (a lazy LINQ pipeline held in IEnumerable<int> read `is
// IEquatable<string>` True) is the SAME lowerTypeTest function, already fixed
// (see type_test_is_string_generic_carrier_and_pipeline_4925_4909_test.cpp).
// Case A/B here only fire when the operand resolves to a builtin COLLECTION
// or a builtin element-type COLLECTION INTERFACE — never IEquatable<T> /
// IComparable<T> — so this fix cannot interact with that path; a control row
// below re-confirms #4909 stays fixed.
//
// MEASURED fail-without: reverting src/codegen/LlvmBackend_Layout.cpp and
// LlvmBackend_impl.hpp to origin/main turns the first four rows below false
// (exact_array_char_operand_matches_array_char_target_spec_12_7 in
// particular reads "false" pre-fix instead of "true" — the exact repro of
// the silent-drop bug) while the two "unchanged" control rows and the #4909
// control already passed before this fix (they pin behavior this fix leaves
// alone, not the bug it fixes).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// §12.7 / §14 (#4926 narrow, Case A, same family / same argument): `a:
// Array<char>` tested directly against `Array<char>` — no erasure at all, so
// the operand's static type IS the target and the test is decided
// statically. Pre-fix: silently dropped, reads `false` unconditionally
// (`is not` therefore wrongly `true`) with no diagnostic. Post-fix: r = 1
// (`is` true, contributing 1; `is not` false, contributing nothing).
BEZEL_TEST(codegen_type_test_4926,
           exact_array_char_operand_matches_array_char_target_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<char> := ['a', 'b']; "
        "    var r := 0; "
        "    if a is Array<char> then r := r + 1; end; "
        "    if a is not Array<char> then r := r + 2; end; "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("1"));
}

// §12.7 / §14 (#4926 narrow, Case A, same family / mismatched argument): the
// operand's static type is `Array<int>` (a ground, non-erased builtin
// collection), tested against target `Array<char>` — same head, DIFFERENT
// generic argument. Bezel generics are invariant (§14), so this is decidable
// as `false` with no runtime check.
BEZEL_TEST(codegen_type_test_4926,
           array_int_operand_vs_array_char_target_answers_false_spec_12_7_14) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3]; "
        "    if a is Array<char> then Console.WriteLine(\"bug-true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("false"));
}

// §12.7 / §14 (#4926 narrow, Case A, different family): `LinkedList<char>`
// and `Array<char>` are two SEALED, mutually exclusive builtin collection
// classes (bz_llist_t vs bz_array_t) — a LinkedList<char> value can never be
// an Array<char> instance, decidable as `false` even though the element
// type matches.
BEZEL_TEST(codegen_type_test_4926,
           linked_list_char_operand_vs_array_char_target_different_family_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var ll: LinkedList<char> := ['a', 'b']; "
        "    if ll is Array<char> then Console.WriteLine(\"bug-true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("false"));
}

// §12.7 / §14 / §12.9 (#4926 narrow — RE-POINTED by #5077): the operand is
// erased to `IEnumerable<char>` whose OWN generic argument (char) is
// statically known and differs from the target's (int). `IEnumerable<out
// T>` is stdlib-COVARIANT (§12.9), so since #5077 this name is no longer one
// of Case B's decisive five (see the code comment in LlvmBackend_Layout.cpp)
// and this row no longer exercises Case B at all — it falls through to the
// `concreteTarget` bail and answers `false` via the #2775 "dropped" warning
// path instead (this gtest oracle cannot observe the warning itself; see
// tests/lit/type_test_covariant_interface_carrier_warns_5077.bzl for that
// half). The VALUE stays `false` either way — char is a value type, and
// §12.9 "variance applies only when the type argument is a reference type"
// — but the MECHANISM changed, so this row is kept as the control that pins
// the value is unaffected by the #5077 narrowing. Case B's decisive path for
// a genuinely INVARIANT interface is pinned separately, immediately below.
BEZEL_TEST(codegen_type_test_4926,
           interface_carrier_mismatched_element_type_answers_false_spec_12_7_14) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<char> := ['a', 'b']; "
        "    var e: IEnumerable<char> := a; "
        "    if e is Array<int> then Console.WriteLine(\"bug-true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("false"));
}

// §12.7 / §14 (#5077 — Case B's decisive path, now restricted to the five
// stdlib-INVARIANT interfaces): the operand is erased to `IList<char>`
// (invariant — no `out`/`in` marker; `IList<T>` has mutating methods, per
// §12.9's own worked example) whose OWN generic argument (char) is
// statically known and differs from the target's (int). Bezel's ordinary
// invariance holds for this name, so `Array<T> : IList<T>` for exactly that
// T — a value assignable to IList<char> can never be an Array<int> instance,
// regardless of which concrete collection backs the interface, decidable
// with no runtime check. This is the positive control that Case B still
// fires for at least one of the five names #5077 kept in, now that
// IEnumerable/IReadOnlyCollection/IReadOnlyList have been carved out above.
BEZEL_TEST(codegen_type_test_4926,
           interface_carrier_invariant_ilist_mismatched_element_type_answers_false_spec_12_7_14_5077) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<char> := ['a', 'b']; "
        "    var e: IList<char> := a; "
        "    if e is Array<int> then Console.WriteLine(\"bug-true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("false"));
}

// §17.4 (#4926 narrow, `as`): a reference-type target's `as` yields the
// operand itself (no unboxing — Array<T> is never boxed) on a Case-A match,
// or null otherwise.
BEZEL_TEST(codegen_type_test_4926,
           as_operator_matches_and_mismatches_spec_17_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<char> := ['a', 'b']; "
        "    var b := a as Array<char>; "
        "    if b <> null then Console.WriteLine(\"nonnull\"); "
        "    else Console.WriteLine(\"null\"); end; "
        "    var ai: Array<int> := [1, 2, 3]; "
        "    var c := ai as Array<char>; "
        "    if c <> null then Console.WriteLine(\"nonnull\"); "
        "    else Console.WriteLine(\"null\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("nonnull\nnull"));
}

// §12.7 (#4926 narrow): a nullable operand (`Array<char>?`) at a matching
// static type is an instance only when non-null — null is an instance of no
// type even at Case A's statically-identical type.
BEZEL_TEST(codegen_type_test_4926,
           nullable_operand_null_is_not_an_instance_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<char>? := null; "
        "    if a is Array<char> then Console.WriteLine(\"true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    var src: Array<char> := ['a', 'b']; "
        "    var b: Array<char>? := src; "
        "    if b is Array<char> then Console.WriteLine(\"true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("false\ntrue"));
}

// §12.7 (#4926 — documents what this narrow fix does NOT decide): a
// matching-element-type INTERFACE carrier erases exactly the information
// Case A/B need. Behavior is UNCHANGED by this fix (still `false`, no
// crash). Still `false` after fix #6776 too, for a different reason: the
// tag now exists on the array, but an interface-typed location holds the
// caller-materialised adapter object, not the array (see the header's
// UPDATE), so the test still takes the #2775 "dropped" bail.
BEZEL_TEST(codegen_type_test_4926,
           interface_carrier_matching_element_type_stays_ambiguous_unchanged_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<char> := ['a', 'b']; "
        "    var e: IEnumerable<char> := a; "
        "    if e is Array<char> then Console.WriteLine(\"true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("false"));
}

// §12.7 / §8.2 (#4926 — Object carrier, DECIDED by fix #6776, v0.340): an
// Object carrier erases the static type, but since #6776 the array itself
// carries its runtime identity (word 0 = `bz_array_vtable`, descriptor =
// this module's `Array<char>` instantiation), so `o is Array<char>` is a
// real runtime test and answers `true`. Before #6776 this row pinned
// `false` as "unchanged, undecidable" (see the header); the flip is the
// issue's own headline repro being answered.
BEZEL_TEST(codegen_type_test_4926,
           object_carrier_decided_by_runtime_tag_spec_12_7_6776) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<char> := ['a', 'b']; "
        "    var o: Object := a; "
        "    if o is Array<char> then Console.WriteLine(\"true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("true"));
}

// §12.7 / §9.6 (#4909 control — same lowerTypeTest function, must stay
// fixed): re-confirms the #4909 fix is untouched by this change. A lazy LINQ
// pipeline held in IEnumerable<int> answers `is IEquatable<string>` False —
// Case A/B do not fire here (IEquatable<T> is neither a builtin collection
// nor a collection-element interface), so this exercises the SAME
// lowerTypeTest call through an unrelated arm.
BEZEL_TEST(codegen_type_test_4926,
           control_4909_lazy_pipeline_is_iequatable_string_still_false_spec_12_7_9_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3]; "
        "    var q: IEnumerable<int> := a.Select(lambda(x) => x * 2); "
        "    if q is IEquatable<string> then Console.WriteLine(\"bug-true\"); "
        "    else Console.WriteLine(\"false\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("false"));
}
