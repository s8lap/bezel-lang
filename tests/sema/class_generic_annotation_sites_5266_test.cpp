// Regression tests for issue #5266 (follow-up to #5210) — spec §14.1
// "Instantiation and its site" and §12.7 "Value-typed operands" (v0.313), and
// §3.6 rule 2.
//
// §14.1: a generic type is instantiated by being NAMED — in a field, local,
// parameter or return annotation, a base clause, a choice payload or
// inline-array element type, or as a type argument of another constructed
// type — and a rule that fires at the instantiation site "fires at every such
// site".  `class G<T>` whose body does `v as IS` (reference target over the
// bare operand) is therefore rejected at EVERY site naming `G<int>`; each
// site reports exactly once.
//
// Oracle: reject rows count the diagnostics carrying the join's own needle
// (exact count, so a missing OR a doubled report fails); accept rows must
// compile and run with exit code 0.

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Number of errors containing `needle`, or -1 if the program ran.
int errorsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    if (r.ran) return -1;
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            ++n;
    return n;
}

const char* kOperand = "`as` OPERAND against the reference target `IS`";
const char* kDefault = "does not satisfy the constraint `where K: value`";
const char* kAsTarget = "is used as an `as` target in the generic body";
const char* kCase = "is used as a `case` scrutinee against a reference-typed";

const std::string kOperandG =
    "module M; interface IS end IS; "
    "class G<T> public method F(v: T): bool "
    "  var o := v as IS; return o <> null; end F; end G; ";
const std::string kMain =
    "class C public static method Main(): int return 0; end Main; end C;";

int operandErrors(const std::string& body) {
    return errorsWith(kOperandG + body, kOperand);
}

}  // namespace

BEZEL_TEST(class_generic_annotation_sites_5266, field_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class H private _g: G<int>; end H; class C public static method Main(): int return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, property_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class H public property P: G<int>? get => null; end; end H; class C public static method Main(): int return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, param_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class H public method M(g: G<int>) end M; end H; class C public static method Main(): int return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, return_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class H public method M(): G<int>? return null; end M; end H; class C public static method Main(): int return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, local_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class C public static method Main(): int var g: G<int>; return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, primary_ctor_param_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class D(g: G<int>) end D; class C public static method Main(): int return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, record_positional_field_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "record R(g: G<int>); class C public static method Main(): int return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, choice_payload_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "choice K Some(g: G<int>), None; end K; class C public static method Main(): int return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, indexer_param_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class H public property [g: G<int>]: int get => 0; end; end H; class C public static method Main(): int return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, lambda_param_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class C public static method Main(): int var f := lambda(g: G<int>) => 1; return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, lambda_return_ascription_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class C public static method Main(): int var f := lambda(): G<int>? => null; return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, nested_array_type_argument_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class C public static method Main(): int var a: Array<G<int>>?; return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, nested_linkedlist_type_argument_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class C public static method Main(): int var a: LinkedList<G<int>>?; return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, nullable_form_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class C public static method Main(): int var g: G<int>?; return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, inline_array_element_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class C public static method Main(): int var a: [2]G<int>?; return 0; end Main; end C;"), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, function_type_param_rejected_once) {
    BEZEL_EXPECT_EQ(operandErrors(
        "class C public static method Main(): int var f: (G<int>) -> int; return 0; end Main; end C;"), 1);
}

// The per-site COUNT rows (annotation + `new`, later-line `new`, two
// annotations: two sites, two diagnostics) live in tests/lit/sema/
// class_generic_annotation_*_two_sites_5266.bzl — the in-process runAot sink
// folds same-text diagnostics, so a gtest count there is not a faithful oracle.

BEZEL_TEST(class_generic_annotation_sites_5266, default_join_field_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; enum Level Low := 1, Mid := 2, High := 3; end Level; enum Z Zero := 0, One := 1; end Z; class Box<K> where K: value public method Get(): K var t := default(K); return t; end Get; end Box; class H private _b: Box<Level>?; end H; class C public static method Main(): int return 0; end Main; end C;", kDefault), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, default_join_local_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; enum Level Low := 1, Mid := 2, High := 3; end Level; enum Z Zero := 0, One := 1; end Z; class Box<K> where K: value public method Get(): K var t := default(K); return t; end Get; end Box; class C public static method Main(): int var b: Box<Level>?; return 0; end Main; end C;", kDefault), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, default_join_zero_enum_annotation_accepted_control) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; enum Level Low := 1, Mid := 2, High := 3; end Level; enum Z Zero := 0, One := 1; end Z; class Box<K> where K: value public method Get(): K var t := default(K); return t; end Get; end Box; class C public static method Main(): int var b: Box<Z>?; return 0; end Main; end C;", kDefault), -1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, as_target_join_field_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; class Box<T> public method Probe(o: Object): int var r := o as T; if r <> null then return 1; end; return 0; end Probe; end Box; class H private _b: Box<[3]int>?; end H; class C public static method Main(): int return 0; end Main; end C;", kAsTarget), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, as_target_join_param_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; class Box<T> public method Probe(o: Object): int var r := o as T; if r <> null then return 1; end; return 0; end Probe; end Box; class H public method M(b: Box<[3]int>) end M; end H; class C public static method Main(): int return 0; end Main; end C;", kAsTarget), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, as_target_join_int_annotation_accepted_control) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; class Box<T> public method Probe(o: Object): int var r := o as T; if r <> null then return 1; end; return 0; end Probe; end Box; class C public static method Main(): int var b: Box<int>?; return 0; end Main; end C;", kAsTarget), -1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, case_binding_join_field_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; interface IS end IS; class G<T> public method F(v: T): int return (case v of var x: IS => 1, _ => 9); end F; end G; class H private _g: G<int>?; end H; class C public static method Main(): int return 0; end Main; end C;", kCase), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, case_binding_join_local_annotation_rejected_once) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; interface IS end IS; class G<T> public method F(v: T): int return (case v of var x: IS => 1, _ => 9); end F; end G; class C public static method Main(): int var g: G<int>?; return 0; end Main; end C;", kCase), 1);
}

BEZEL_TEST(class_generic_annotation_sites_5266, case_binding_join_string_annotation_accepted_control) {
    BEZEL_EXPECT_EQ(errorsWith(
        "module M; interface IS end IS; class G<T> public method F(v: T): int return (case v of var x: IS => 1, _ => 9); end F; end G; class C public static method Main(): int var g: G<string>?; return 0; end Main; end C;", kCase), -1);
}

// Accept control: reference substitution at every annotation position compiles
// and runs.
BEZEL_TEST(class_generic_annotation_sites_5266, reference_substitution_accepted) {
    auto r = runAot(
        "module M; interface IS end IS; "
        "class Impl : IS public constructor() end; end Impl; "
        "class G<T> public method F(v: T): bool "
        "  var o := v as IS; return o <> null; end F; end G; "
        "class H(p: G<Impl>?) private _g: G<Impl>?; "
        "  public method M(g: G<Impl>?): G<Impl>? return null; end M; end H; "
        "class C public static method Main(): int "
        "  var g: G<Impl>?; var a: Array<G<Impl>>?; "
        "  var f := lambda(x: G<Impl>?) => 1; return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
