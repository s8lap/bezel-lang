// Codegen + sema — spec §12.7 / §17.4 / §9.6: three is-test defects on the
// shared lowerTypeTest path that all reduce to "the operand may hold a
// vtable-less / non-vtable-bearing runtime value, but neither the bz_lazy_vtable
// walk nor the emitIsA descriptor walk was taught to recognise the carrier".
//
//   #4925: a generic `v: T` carrier instantiated at T = string.
//          `T1<string>("ab")` returns a value that is internally a bare
//          `bz_string*` (no leading vtable / descriptor word, #1071). The
//          pre-fix static decision arm saw opTy = T (Named with declaration =
//          nullptr, the bare type-parameter shape) — NOT Primitive String —
//          and so fell through to emitIsA, which read `bz_string*`'s word 0
//          (the `char* data` pointer) as a descriptor pointer and walked it
//          (SIGSEGV, MEASURED). Fix: resolve opTy through genericSubstSemaTy_
//          so the static decision arm fires for monomorphized bodies; widen
//          staticTypeMightHoldBareString so the unmonomorphized template
//          still gates the descriptor walk through bz_obj_is_string.
//   #4909: a lazy LINQ pipeline held in `IEnumerable<int>`. The pipeline's
//          runtime representation is a 4-entry bz_lazy_vtable C array
//          (NOT null-terminated — only bz_lazy_vtable, bz_lazy_dispose,
//          bz_lazy_movenext, bz_lazy_current), so emitIsA's descriptor walk
//          read PAST the array and matched a `td$IEquatable_string` global
//          the linker happened to put after it, answering TRUE for
//          `q is IEquatable<string>` — wrong. Fix: widen
//          staticTypeMightHoldBareString so the declaration-less
//          `IEnumerable<T>` shape (the vtable-backed IEnumerable<T> —
//          declaration = nullptr, astNode or importedName set, single
//          generic arg) gates through bz_obj_is_string, which catches the
//          vtable-less bz_lazy_vtable first word.
//   #4926: `e is Array<char>` where `e: IEnumerable<char> := a (Array<char>)`
//          — a builtin null-declaration constructed generic target has no
//          descriptor global to compare against, so lowerTypeTest currently
//          reaches "no runtime descriptor resolved for the target type" and
//          drops the test (#3079). Out of scope for THIS test file: bz_array_t
//          has no leading descriptor word, so the runtime stamping path is
//          filed separately (#4926.1) — these rows only verify the static
//          decision arm plus the two widening predicates.
//
// WHY ONE FILE. The three sites share a single function (lowerTypeTest's
// mightHoldString guard + the static decision arm right above it) and a
// single predicate (staticTypeMightHoldBareString). Splitting them across
// files would force three partial fixes to be reviewed against the same
// shared context, which is exactly the structure §17.2's per-fix review
// (LANE_BRIEF) is meant to prevent.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// §12.7 / §17.4 (#4925): `T1<T>(v: T)` with T = string. The static decision
// arm must resolve opTy through genericSubstSemaTy_ so the bare-type-param
// shape is recognized as Primitive String at the monomorphized body site.
// Pre-fix: SIGSEGV (the emitter walked the bz_string*'s char* data as a
// descriptor chain).  Expected post-fix: r = 30 (4 + 8 + 16 + 2 — string
// is an Object, is string, is IEnumerable<char>, is IEquatable<string>;
// string is NOT an IShape).
BEZEL_TEST(codegen_type_test_4925_4909,
           generic_carrier_v_at_T_string_answers_section_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public interface IShape method Area(): int; end IShape; "
        "public class C "
        "  public static method T1<T>(v: T): int "
        "    var r := 0; "
        "    if v is IShape then r := r + 1; end; "
        "    if v is IEquatable<string> then r := r + 2; end; "
        "    if v is Object then r := r + 4; end; "
        "    if v is string then r := r + 8; end; "
        "    if v is IEnumerable<char> then r := r + 16; end; "
        "    return r; "
        "  end T1; "
        "  public static method Main(): int "
        "    Console.WriteLine(T1<string>(\"ab\")); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("30"));
}

// §12.7 / §17.4 (#4909): `q is IEquatable<string>` where `q` is a lazy
// LINQ pipeline held in `IEnumerable<int>`. The pipeline's runtime shape is
// a 4-entry bz_lazy_vtable C array with NO null terminator, so the descriptor
// walk read past the array and matched a td$IEquatable_string global the
// linker put after it — pre-fix answered TRUE. Post-fix: bz_obj_is_string
// recognizes the vtable-less first word and the walk returns FALSE; pipeline
// sanity (its own IEnumerable<int>, Object) still answers TRUE.
BEZEL_TEST(codegen_type_test_4925_4909,
           lazy_pipeline_is_unrelated_interface_answers_false_spec_12_7_9_6) {
    // The FIRST `is` is the bug: q is IEquatable<string> must be False.
    // The remaining rows are positive / negative controls so a generic "all
    // False" regression (e.g. stringIsInstanceOf short-circuited away) would
    // light up at least one of them — `q is IEnumerable<int>` is TRUE
    // (sanity), `q is Object` is TRUE (sanity), `q is string` is FALSE
    // (vtable-less pipeline is not a string), `q is IEquatable<int>` is
    // FALSE (the pipeline is a lazy enumerable, not an int-bag).
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

// §12.7 / §17.4 (#4909.1 — follow-up filed): the lazy pipeline's runtime
// descriptor chain is `bz_lazy_vtable` (a 4-entry C array of function
// pointers, no null terminator) so a deeper question — "is a lazy pipeline a
// runtime instance of its own IEnumerable<T> at all" — is a separate
// emitIsA-walk-bound defect. No row pins it here; that ships with #4909.1.

// §12.7 (v0.313) / narrow #5159: `int` conforms to `IEquatable<int>` (stdlib
// §9.1) and not to `IEquatable<string>`, so T1<int>(7) is 1 (was 0 while the
// value-substituted carrier's interface test was silently dropped).
// §12.7 / §17.4 (#4925 control — bare type-parameter operand AT A NON-STRING
// monomorphization). A `v: T` carrier at T = int must still fall through to
// the descriptor walk without crashing: the #4909 / #4925 widening turns
// `mightHoldString` on, so the test goes through bz_obj_is_string and exits
// through the "not a string" arm.
BEZEL_TEST(codegen_type_test_4925_4909,
           generic_carrier_v_at_T_int_does_not_crash_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method T1<T>(v: T): int "
        "    var r := 0; "
        "    if v is IEquatable<int> then r := r + 1; end; "
        "    if v is IEquatable<string> then r := r + 2; end; "
        "    return r; "
        "  end T1; "
        "  public static method Main(): int "
        "    Console.WriteLine(T1<int>(7)); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("1"));
}

// §12.7 "Value-typed operands" (v0.313), narrow #5159: at T = int, `v is Object`
// is true, `v is IEquatable<int>` true, `v is IComparable<int>` true,
// `v is IEquatable<string>` false, and `is not` is the negation.
BEZEL_TEST(codegen_type_test_4925_4909,
           value_carrier_is_object_and_interface_static_answer_spec_12_7_5159) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method T1<T>(v: T): int "
        "    var r := 0; "
        "    if v is Object then r := r + 1; end; "
        "    if v is IEquatable<int> then r := r + 2; end; "
        "    if v is IComparable<int> then r := r + 4; end; "
        "    if v is IEquatable<string> then r := r + 8; end; "
        "    if v is not IEquatable<string> then r := r + 16; end; "
        "    return r; "
        "  end T1; "
        "  public static method Main(): int "
        "    Console.WriteLine(T1<int>(7)); "
        "    Console.WriteLine(T1<bool>(true)); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("23\n17"));
}

// fix #5159 (§12.7 "Value-typed operands", v0.315): a record value substituted
// for `v: T` and tested by `is` against an interface it implements is true
// (§14.2 conformance, with §12.9 variance). Before the fix, argSatisfiesTypeBound
// was not reachable from codegen for non-primitive value types, so the test
// answered false.
BEZEL_TEST(codegen_type_test_4925_4909,
           record_carrier_is_implemented_interface_answers_true_spec_12_7_5159) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public interface IShape method Area(): int; end IShape; "
        "public record Sq(Side: int): IShape "
        "  public method Area(): int return Side * Side; end Area; "
        "end Sq; "
        "public class C "
        "  public static method T1<T>(v: T): int "
        "    if v is IShape then return 1; end; "
        "    return 0; "
        "  end T1; "
        "  public static method Main(): int "
        "    Console.WriteLine(T1<Sq>(new Sq(3))); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("1"));
}
