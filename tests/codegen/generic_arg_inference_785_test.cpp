// fix #785 / §14.1: a generic method's type arguments are inferred by
// STRUCTURAL unification of a generic-typed parameter against the argument —
// `First<A, B>(p: Pair<A, B>): A` called with `p: Pair<int, string>` binds
// A=int, B=string, so no target type is required and the result is typed A.
//
// Three flat lookups had to become recursive: (1) inferReturnTargetTypeArgs's
// boundByArg ("does this call need a target?"), (2) typeCall's argument-inferred
// argSubst (which substitutes the call's RESULT type — the one that left `var q
// := First(p)` typed `?`), and (3) codegen's method argument-inference (binds
// the type params for the body). All three only matched a top-level bare `p: T`
// before; now they recurse into `Pair<A, B>`.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
const char* kPair = "module M; record Pair<A, B>(a: A, b: B); ";
}  // namespace

// §14.1: the repro — `var q := C.First(p)` with no target type; A=int is
// inferred from `p: Pair<int, int>` and q is typed int (used with `mod`).
BEZEL_TEST(generic_arg_inference_785, infers_return_from_generic_param_no_target) {
    BEZEL_EXPECT_EQ(runFull(std::string(kPair) +
        "class C "
        "  public static method First<A, B>(p: Pair<A, B>): A return p.a; end First; "
        "  public static method Main(): int "
        "    var p := new Pair<int, int>(3, 7); "
        "    var q := C.First(p); "
        "    if q = 3 then return 0; end; "
        "    return q mod 100; "
        "  end Main; "
        "end C;"), 0);
}

// §14.1: the inferred int result participates in arithmetic — q is genuinely int.
BEZEL_TEST(generic_arg_inference_785, inferred_int_result_is_arithmetic) {
    BEZEL_EXPECT_EQ(runFull(std::string(kPair) +
        "class C "
        "  public static method First<A, B>(p: Pair<A, B>): A return p.a; end First; "
        "  public static method Main(): int "
        "    var p := new Pair<int, int>(3, 7); "
        "    var q := C.First(p); "
        "    return q + 100; "  // 103
        "  end Main; "
        "end C;"), 103);
}

// §14.1: NON-int type args — A=string is bound (and codegen lowers it to a
// pointer, not the i32 placeholder). `s` is typed string; `.Length` resolves.
BEZEL_TEST(generic_arg_inference_785, infers_string_type_arg) {
    BEZEL_EXPECT_EQ(runFull(std::string(kPair) +
        "class C "
        "  public static method First<A, B>(p: Pair<A, B>): A return p.a; end First; "
        "  public static method Main(): int "
        "    var p := new Pair<string, int>(\"hi\", 9); "
        "    var s := C.First(p); "
        "    return s.Length; "  // 2
        "  end Main; "
        "end C;"), 2);
}

// §14.1: the SECOND type parameter is bound too — `Second` returns B, inferred
// int from `Pair<string, int>`.
BEZEL_TEST(generic_arg_inference_785, infers_second_type_param) {
    BEZEL_EXPECT_EQ(runFull(std::string(kPair) +
        "class C "
        "  public static method Second<A, B>(p: Pair<A, B>): B return p.b; end Second; "
        "  public static method Main(): int "
        "    var p := new Pair<string, int>(\"hi\", 42); "
        "    return C.Second(p); "  // 42
        "  end Main; "
        "end C;"), 42);
}

// §14.1 regression guard: a plain bare-param generic call `Id<T>(x: T): T` still
// infers T from the argument (the structural change must not regress it).
BEZEL_TEST(generic_arg_inference_785, bare_param_inference_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Id<T>(x: T): T return x; end Id; "
        "  public static method Main(): int "
        "    var q := C.Id(55); "
        "    return q; "  // 55
        "  end Main; "
        "end C;"), 55);
}

// §14.1 / #1162: a method type param INFERRED to a builtin COLLECTION
// (Array<int>) must bind to that collection's lowered type (a heap pointer),
// exactly as the explicit `Echo<Array<int>>(a)` form does. Before the fix the
// inferred binding was skipped (a builtin collection is a decl-less Named, like
// an unresolved type param), so `U` lowered to the i32 placeholder and the call
// was typed `call i32 @Echo(ptr …)` while the monomorphized body returned ptr →
// IR verification failure. Here `Echo(a)` returns the Array; `.Count` is 3.
BEZEL_TEST(generic_arg_inference_785, infers_builtin_collection_array_1162) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Echo<U>(x: U): U => x; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(5); a.Add(6); a.Add(7); "
        "    var r := Echo(a); "
        "    return r.Count; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// §14.1 / #1162 regression guard: the EXPLICIT type-arg form is the contract the
// inferred form must match — it must keep returning 3.
BEZEL_TEST(generic_arg_inference_785, explicit_builtin_collection_array_1162) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Echo<U>(x: U): U => x; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(5); a.Add(6); a.Add(7); "
        "    var r := Echo<Array<int>>(a); "
        "    return r.Count; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// §14.1 / #1162: a Dictionary is inferred the same way (heap-pointer builtin).
BEZEL_TEST(generic_arg_inference_785, infers_builtin_collection_dictionary_1162) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Echo<U>(x: U): U => x; "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int, int>(); d.Add(1, 10); d.Add(2, 20); "
        "    var r := Echo(d); "
        "    return r.Count; "  // 2
        "  end Main; "
        "end C;"), 2);
}

// §14.1 / #1162 regression guard: a NON-collection scalar inference is
// unaffected — `Echo(5)` still returns 5 (the skip path for genuine type-param /
// null-typed candidates must remain intact).
BEZEL_TEST(generic_arg_inference_785, scalar_inference_unaffected_1162) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Echo<U>(x: U): U => x; "
        "  public static method Main(): int "
        "    return Echo(5); "  // 5
        "  end Main; "
        "end C;"), 5);
}
