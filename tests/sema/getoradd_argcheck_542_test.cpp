// Spec-derived tests for issue #542 — Dictionary<K,V>.GetOrAdd(key, factory)
// must reject a non-function second argument with a clean sema diagnostic
// instead of silently passing a non-closure into codegen (stdlib §4.8 / §4.9).
//
// Per stdlib §4.8:
//   public method GetOrAdd(key: K, factory: (K) -> V): V;
// Per stdlib §4.9 (OwningDictionary):
//   public method GetOrAdd(key: K, factory: (K) -> owned V): V;
//
// Before fix (#542): `d.GetOrAdd("a", 99)` emitted no sema error; codegen
// treated the int literal 99 as a closure pointer — `load ptr, i32 99, align 8`
// — producing malformed IR / a crash.
//
// After fix: sema emits a clear "factory" type-mismatch error when the second
// argument is not a function/lambda, citing §4.8 / #542.
//
// Tests:
//   (a) int literal as factory → error, message mentions "factory" or "function"
//   (b) string literal as factory → error
//   (c) bool literal as factory → error
//   (d) valid lambda factory `(k) => 7` → no error
//   (e) OwningDictionary: same int-literal rejection → error
//   (f) OwningDictionary: valid lambda → no error
//   (g) LINQ Where/Select with valid lambdas still compile fine (control)
//   (h) Array<T>.Sorted(comparer) with valid comparer still compiles fine

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

bool anyDiagContains(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos) return true;
    return false;
}

// Wrap a method body in a module/class that has:
//   d  : Dictionary<string, int>
//   od : OwningDictionary<string, Val>  (Val is a class; §4.2/#1327 rejects int)
//   arr: Array<int>
//   n  : int
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class Val end Val; "       // minimal class for OwningDictionary value
        "class C "
        "public method M("
        "  d: Dictionary<string, int>, "
        "  od: OwningDictionary<string, Val>, "
        "  arr: Array<int>, "
        "  n: int"
        ") ") + body + " end M; end C;";
}

} // namespace

// ===========================================================================
// §4.8 — GetOrAdd on Dictionary<K,V>: factory must be a function/lambda
// ===========================================================================

// §4.8 / #542: passing an int literal as the factory argument must produce
// a sema error. The diagnostic must mention "factory" or "function" so the
// user understands what is expected.
BEZEL_TEST(sema_getoradd_argcheck_542,
           dict_getoradd_int_factory_arg_is_rejected_spec_4_8) {
    auto r = parseSemaAll(wrap("var v: int := d.GetOrAdd(\"a\", 99);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The diagnostic must mention what the argument should be — a factory/function.
    bool mentionsFactory = anyDiagContains(r, "factory") ||
                           anyDiagContains(r, "function") ||
                           anyDiagContains(r, "Function");
    BEZEL_EXPECT_TRUE(mentionsFactory);
}

// §4.8 / #542: passing a string literal as the factory argument must also
// produce a sema error.
BEZEL_TEST(sema_getoradd_argcheck_542,
           dict_getoradd_string_factory_arg_is_rejected_spec_4_8) {
    auto r = parseSemaAll(wrap("var v: int := d.GetOrAdd(\"a\", \"str\");"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §4.8 / #542: passing a bool literal as the factory argument must produce
// a sema error.
BEZEL_TEST(sema_getoradd_argcheck_542,
           dict_getoradd_bool_factory_arg_is_rejected_spec_4_8) {
    auto r = parseSemaAll(wrap("var v: int := d.GetOrAdd(\"a\", true);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §4.8 / #542: a valid lambda factory `(k) => 7` — the factory is a function
// and the call is well-formed. Must produce no errors.
BEZEL_TEST(sema_getoradd_argcheck_542,
           dict_getoradd_valid_lambda_factory_accepted_spec_4_8) {
    auto r = parseSemaAll(wrap("var v: int := d.GetOrAdd(\"a\", (k) => 7);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §4.9 — GetOrAdd on OwningDictionary<K,V>: same factory requirement
// ===========================================================================

// §4.9 / #542: passing an int literal as the factory argument to
// OwningDictionary.GetOrAdd must also be rejected.
// od is OwningDictionary<string, Val>; factory must be (string) -> owned Val.
BEZEL_TEST(sema_getoradd_argcheck_542,
           owned_dict_getoradd_int_factory_arg_is_rejected_spec_4_9) {
    auto r = parseSemaAll(wrap("_ := od.GetOrAdd(\"a\", 99);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §4.9 / #542: a valid lambda factory on OwningDictionary.GetOrAdd must be
// accepted without errors.
// od is OwningDictionary<string, Val>; factory returns owned Val.
BEZEL_TEST(sema_getoradd_argcheck_542,
           owned_dict_getoradd_valid_lambda_accepted_spec_4_9) {
    auto r = parseSemaAll(wrap("var v: Val := od.GetOrAdd(\"a\", (k) => new Val());"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// CONTROL — sibling collection-method calls with function params still work
// ===========================================================================

// §5 / #542: `arr.Where((x) => x > 0)` — a LINQ Where with a valid predicate
// lambda. Must not be broken by the factory-arg check.
BEZEL_TEST(sema_getoradd_argcheck_542,
           array_where_with_valid_predicate_accepted_spec_5) {
    auto r = parseSemaAll(wrap("var e := arr.Where((x) => x > 0);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5 / #542: `arr.Select((x) => x * 2)` — a LINQ Select with a valid
// selector lambda. Must not be broken by the factory-arg check.
BEZEL_TEST(sema_getoradd_argcheck_542,
           array_select_with_valid_selector_accepted_spec_5) {
    auto r = parseSemaAll(wrap("var e := arr.Select((x) => x * 2);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §3 / #472 / #542: `arr.Sorted((a, b) => a - b)` with a valid comparer.
// Must not be broken by the factory-arg check.
BEZEL_TEST(sema_getoradd_argcheck_542,
           array_sorted_with_valid_comparer_accepted_spec_3) {
    auto r = parseSemaAll(wrap("var s := arr.Sorted((a, b) => a - b);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Sibling holes — Array<T>.Sort(comparer) and Array<T>.Sorted(comparer)
// with non-function arguments (#542 audit)
// ===========================================================================

// §3 / #542: `arr.Sort(99)` — Sort(comparer) with an int literal must be
// rejected by sema. Without the check, codegen treats 99 as a closure pointer
// → malformed IR / crash.
BEZEL_TEST(sema_getoradd_argcheck_542,
           array_sort_int_comparer_arg_is_rejected_spec_3) {
    auto r = parseSemaAll(wrap("arr.Sort(99);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §3 / #542: `arr.Sort(true)` — Sort(comparer) with a bool must be rejected.
BEZEL_TEST(sema_getoradd_argcheck_542,
           array_sort_bool_comparer_arg_is_rejected_spec_3) {
    auto r = parseSemaAll(wrap("arr.Sort(true);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §3 / #472 / #542: `arr.Sorted(99)` — Sorted(comparer) with an int literal
// must be rejected by sema.  Without the check, codegen treats 99 as a
// closure pointer → malformed IR / crash.
BEZEL_TEST(sema_getoradd_argcheck_542,
           array_sorted_int_comparer_arg_is_rejected_spec_3) {
    auto r = parseSemaAll(wrap("var s := arr.Sorted(99);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §3 / #542: `arr.Sort((a, b) => a - b)` with a valid comparer.
// Must compile clean — the Sort(comparer) check must not reject valid lambdas.
BEZEL_TEST(sema_getoradd_argcheck_542,
           array_sort_with_valid_comparer_accepted_spec_3) {
    auto r = parseSemaAll(wrap("arr.Sort((a, b) => a - b);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
