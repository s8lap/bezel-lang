// Spec-derived tests for the §31.1 / §17.12 throw-operand TYPE rule
// (issues #1568, #1569; docs v0.182).
//
// §31.1 (v0.182): "`throw expr` requires `expr` to be an owned instance of a
// class type — any class, deliberately *not* restricted to `Exception`-derived
// classes (codifying the #771 ruling). ... The operand's static type must be a
// class type; values of primitive, `string`, record, enum, inline-array, unit,
// or function type cannot be thrown — a compile error at the throw site — and an
// `Object`- or interface-typed operand is likewise rejected (its referent may be
// a boxed value, which is not throwable; narrow to the class first)."
//
// #1568 records that the Exception-derivation question stays as #771 ruled: any
// class is throwable. So the TYPE rule accepts BOTH an `Exception`-derived class
// AND a plain non-`Exception` class; it rejects a primitive, `string`, `Object`,
// an interface, a record, an enum, and a function value. This file exercises the
// TYPE rule in the sema pass (the borrowed-vs-owned OPERAND rule is an ownership
// rule, tested in tests/ownership/throw_owned_operand_test.cpp).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// A module carrying: an interface `IShape` and its class implementer `Circle`;
// a plain non-`Exception` class `Widget`; an `Exception`-derived class
// `AppError`; a record `Point`; and an enum `Color`. `Run()` hosts the body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "interface IShape end IShape; "
        "class Circle : IShape end Circle; "
        "class Widget end Widget; "
        "class AppError : Exception "
        "  public constructor() base(\"e\"); end constructor; "
        "end AppError; "
        "record Point(x: int); "
        "enum Color Red, Green; end Color; "
        "class C public static method Run() ") + body + " end Run; end C;";
}

} // namespace

// ── ACCEPTED — any class is throwable (§31.1 / #771 / #1568) ──────────────────

// §31.1: a plain non-`Exception` class instance is a valid throw operand — the
// #771 ruling that throw is not restricted to `Exception`-derived types stands
// (v0.182, #1568). ACCEPTED.
BEZEL_TEST(sema_throw_operand_type, non_exception_class_ok_31_1) {
    auto r = parseSemaAll(wrap("throw new Widget();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.1: an `Exception`-derived class instance is a valid throw operand (the
// idiomatic case). ACCEPTED.
BEZEL_TEST(sema_throw_operand_type, exception_derived_class_ok_31_1) {
    auto r = parseSemaAll(wrap("throw new AppError();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── REJECTED — a non-class operand is not throwable (§31.1) ───────────────────

// §31.1: a `string` operand cannot be thrown. REJECTED.
BEZEL_TEST(sema_throw_operand_type, string_rejected_31_1) {
    auto r = parseSemaAll(wrap("throw \"oops\";"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.1: a primitive (`int`) operand cannot be thrown. REJECTED.
BEZEL_TEST(sema_throw_operand_type, int_primitive_rejected_31_1) {
    auto r = parseSemaAll(wrap("throw 42;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.1: a record operand is a value type, not throwable. REJECTED.
BEZEL_TEST(sema_throw_operand_type, record_rejected_31_1) {
    auto r = parseSemaAll(wrap("var p := new Point(1); throw p;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.1: an enum operand is a value type, not throwable. REJECTED.
BEZEL_TEST(sema_throw_operand_type, enum_rejected_31_1) {
    auto r = parseSemaAll(wrap("throw Color.Red;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.1: an interface-typed operand is rejected — its referent may be a boxed
// value, which is not throwable; narrow to the class first. REJECTED.
BEZEL_TEST(sema_throw_operand_type, interface_typed_rejected_31_1) {
    auto r = parseSemaAll(wrap("var sh: IShape := new Circle(); throw sh;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.1: an `Object`-typed operand is rejected — same boxed-referent reason as
// the interface case; narrow to the class first. REJECTED.
BEZEL_TEST(sema_throw_operand_type, object_typed_rejected_31_1) {
    auto r = parseSemaAll(wrap("var o: Object := new Widget(); throw o;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
