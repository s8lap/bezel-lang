// Spec-derived tests for §12.9/§14.2/§17.4 (#2006, v0.197): the erasure guard on
// the PLAIN `is`/`as` EXPRESSION path (src/sema/ExprTypecheck_Exprs.cpp).
//
// The case-binding path already consults `typeArgRuntimeReified` and rejects a
// type test whose differing type argument is a still-erased #1939 kind (#1927/
// #1946/#2001). The plain `is`/`as` expression path did NOT: it typed the result
// unconditionally and reached the collapsed per-monomorphization descriptor.
// Because appendDescKey does not encode a function type's identity (Kind::Function
// -> "F" at any depth), `Wrap<(int)->int> is Wrap<(string)->string>` returned a
// false-positive TRUE at runtime, and the follow-on `as` reinterpreted one
// signature as another -> type confusion (the same memory-unsafe class as #2001's
// inline-array-size collision, just for function-typed arguments).
//
// #2006 brings the plain path to PARITY with the case-binding path: a type test
// against a CONSTRUCTED generic whose type argument is runtime-ERASED (a function
// type, an inline-array-of-function-element, ...) is REJECTED at compile time.
// The reified cases stay ACCEPTED — post-#2001 a folded inline-array argument is
// runtime-distinguishable, and a bare type parameter resolves to a concrete
// argument at monomorphization (#1843's `o is Box<Box<T>>`), so neither is
// over-rejected.

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

// True iff an ERROR diagnostic carries the #2006 plain-path erasure reason. The
// message is unique to this check: it names the "`is`/`as` target" (the
// case-binding #1946 diagnostic names the "binding pattern type"), so the two
// remain distinguishable even in a program that has both.
bool hasPlainErasedError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`is`/`as` target is not runtime-distinguishable") !=
                std::string::npos)
            return true;
    }
    return false;
}

// A phantom `Wrap<W>` (its W is only a type argument, never stored — isolates the
// descriptor-key recursion), a concrete `Foo`, and a `Box<U>` wrapper, followed by
// a body inside `class Program`. Reused across cases.
std::string wrap(const std::string& body) {
    return "module M;\n"
           "class Wrap<W>\nend Wrap;\n"
           "class Foo\nend Foo;\n"
           "class Box<U>\nend Box;\n"
           "class Program\n" +
           body + "end Program;\n";
}

}  // namespace

// ---- REJECTED: a function-typed argument is runtime-ERASED (the headline) ------

// (1) `is`: the exact reported false positive. `o is Wrap<(int) -> int>` reaches
// the collapsed descriptor; the runtime test cannot tell it from
// `Wrap<(string) -> string>` (FAILS WITHOUT the fix — the plain path accepted it,
// so it compiled and returned a false-positive TRUE at runtime). REJECTED.
BEZEL_TEST(is_as_type_test_erased_2006, is_function_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    return (if o is Wrap<(int) -> int> then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasPlainErasedError(r));
}

// (2) `as`: the follow-on cast is the memory-unsafe half — reinterpreting an
// `(int)->int` value through a `(string)->string` signature. REJECTED.
BEZEL_TEST(is_as_type_test_erased_2006, as_function_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var x := o as Wrap<(int) -> int>;\n"
        "    return (if x = null then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasPlainErasedError(r));
}

// (3) NESTED function-typed argument: the erasure must be caught at any depth.
// `Wrap<Wrap<(int) -> int>>` reifies its outer Named but its inner argument is a
// function type, so the descriptor collapses. REJECTED (pins the recursion).
BEZEL_TEST(is_as_type_test_erased_2006, is_nested_function_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    return (if o is Wrap<Wrap<(int) -> int>> then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasPlainErasedError(r));
}

// ---- ACCEPTED: reified arguments stay admitted (no over-rejection) -------------

// (4) A concrete PRIMITIVE argument is runtime-reified. `o is Wrap<int>` compiles
// clean — the fix must not reject the common case.
BEZEL_TEST(is_as_type_test_erased_2006, is_primitive_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    return (if o is Wrap<int> then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasPlainErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (5) A concrete NAMED-class argument is reified. `o is Wrap<Foo>` — ACCEPTED.
BEZEL_TEST(is_as_type_test_erased_2006, is_named_class_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    return (if o is Wrap<Foo> then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasPlainErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) A NESTED reified argument: `o is Wrap<Wrap<int>>` — reifies recursively
// through the concrete inner primitive. ACCEPTED.
BEZEL_TEST(is_as_type_test_erased_2006, is_nested_reified_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    return (if o is Wrap<Wrap<int>> then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasPlainErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) A FOLDED inline-array argument is runtime-distinguishable post-#2001 (the
// descriptor key encodes N). `o is Wrap<[3]int>` — ACCEPTED (the guard must trust
// `typeArgRuntimeReified`, which now admits a folded inline array; the #2001
// admission must not be undone here).
BEZEL_TEST(is_as_type_test_erased_2006, is_inline_array_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    return (if o is Wrap<[3]int> then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasPlainErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (8) The `as` form of a reified argument stays admitted too. ACCEPTED.
BEZEL_TEST(is_as_type_test_erased_2006, as_reified_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var x := o as Wrap<int>;\n"
        "    return (if x = null then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasPlainErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (9) NON-GENERIC `is` (no type arguments) is untouched — `o is Foo`. ACCEPTED.
BEZEL_TEST(is_as_type_test_erased_2006, is_nongeneric_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    return (if o is Foo then 1 else 0);\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasPlainErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (10) A BARE TYPE PARAMETER argument in a generic body resolves to a concrete
// argument at monomorphization, so it is NOT statically erased — the same
// conservative-accept posture the case-binding path takes (#1843's
// `o is Box<Box<T>>`). `o is Wrap<Wrap<T>>` inside `Chk<T>` must stay ACCEPTED
// (the over-rejection guard: the guard must hold off a type-variable target).
BEZEL_TEST(is_as_type_test_erased_2006, is_bare_type_parameter_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method Chk<T>(o: Object): int\n"
        "    return (if o is Wrap<Wrap<T>> then 1 else 0);\n"
        "  end Chk;\n"));
    BEZEL_EXPECT_FALSE(hasPlainErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
