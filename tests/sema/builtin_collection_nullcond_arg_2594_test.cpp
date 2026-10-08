// Spec-derived regression test for issue #2594.
//
// §7 (Nullability) / §7.5 (Null-conditional access `?.`) / stdlib §4.4–§4.10,
// v0.236: the builtin-collection §7 argument gate added by #2582 keyed on a
// `dynamic_cast<const ast::MemberAccessExpr*>` callee, so the NULL-CONDITIONAL
// call form `coll?.M(arg)` — a distinct `NullCondMemberExpr` node with the
// same `target`/`member` fields — was never checked at all. An un-narrowed
// `int?` argument was therefore rejected on `coll.IndexOf(n)` but silently
// ACCEPTED on `coll?.IndexOf(n)`: the same §7 hole on a parallel Expr kind.
// #2581 had already closed this for the string and ReadOnlyStringSpan gates
// (which share the `builtinInstanceCallee` extractor); the collection gate was
// the remaining MemberAccessExpr-only site.
//
// §7.5 governs the RECEIVER's nullability only — it does not widen a
// parameter's declared type — so the argument stays under the ordinary §7
// assignability rule and `coll?.IndexOf(7)` must keep compiling.
//
// This is a REJECTION fix, so each negative case asserts on diagnostic TEXT
// (not the coarse rejects() helper — runAot's WIFSIGNALED path synthesizes an
// Error diagnostic for a runtime SIGSEGV/ICE too, so it cannot distinguish a
// genuine reject from a crash). The positive cases guard against false
// rejection of working null-conditional code.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §7 "nullable -> non-nullable" assignability diagnostic fragment shared by
// the user-method path and the builtin-collection path.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2594 / §7 + §7.5 / stdlib §4.4: the issue's repro — `arr?.IndexOf(n)` on a
// nullable `Array<int>?` receiver with an un-narrowed `int?` argument. Before
// the fix this compiled clean (exit 0, no diagnostic) while the plain
// `arr.IndexOf(n)` was rejected.
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_array_indexof_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(arr: Array<int>?): int? "
        "    return arr?.IndexOf(N()); "  // N(): int? — un-narrowed, on `?.`
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.IndexOf"));
    BEZEL_EXPECT_TRUE(hasError(r, "(\xC2\xA7""7)"));
}

// #2594 / stdlib §4.4: `arr?.Contains(n)` — the second search member of the
// Array family on the null-conditional path.
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_array_contains_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(arr: Array<int>?): bool? "
        "    return arr?.Contains(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.Contains"));
}

// #2594 / stdlib §4.8: the KEYED family on the null-conditional path —
// `dict?.ContainsKey(k)` checks the un-narrowed `int?` against K.
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_dictionary_containskey_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(d: Dictionary<int, string>?): bool? "
        "    return d?.ContainsKey(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Dictionary<T>.ContainsKey"));
}

// #2594 / stdlib §4.10: the Set family's borrowing `Add(item: T)` on the
// null-conditional path (a MUTATOR position, not just a query).
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_set_add_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(s: Set<int>?) "
        "    s?.Add(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Set<T>.Add"));
}

// ---- Positive controls (guard against false rejection of working code) ----

// #2594 / §7.5: a legitimate null-conditional call with a PLAIN non-nullable
// argument still compiles. §7.5's `?.` concerns the receiver, not the
// argument — the gate must check the ARG only.
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_indexof_accepts_plain_int) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(arr: Array<int>?): int? "
        "    return arr?.IndexOf(7); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2594 / §7.4: a NARROWED `int?` (proved non-null by a null-check) IS
// assignable to the non-nullable `item: T` parameter on the `?.` path too —
// the narrowing exception lives inside checkAssignable and is reused unchanged.
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_indexof_accepts_narrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(arr: Array<int>?): int? "
        "    var n: int? := 5; "
        "    if n = null then return -1; end; "  // narrows n non-null below
        "    return arr?.IndexOf(n); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2594 / §7 + §7.10: when the element type ITSELF is nullable —
// `Array<int?>` declares `IndexOf(item: int?)` — an un-narrowed `int?`
// argument is admissible on the `?.` path exactly as on the plain path. This
// is the case a naive "reject any nullable argument" gate would break.
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_indexof_accepts_nullable_element_collection) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(arr: Array<int?>?): int? "
        "    return arr?.IndexOf(N()); "  // Array<int?>.IndexOf(item: int?) — OK
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2594 / stdlib §5: the LINQ `Take(count: int)` operator is a NAME COLLISION
// with `OwningSet<T>.Take(query: T)`. #2582 gated `Take` to OwningSet so that
// `arr.Take(2)` is not checked against T; extending the gate to the `?.` form
// must not reintroduce that false reject.
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_linq_take_count_is_not_an_item_slot) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(arr: Array<string>?) "
        "    var t := arr?.Take(2); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2594 / §22.3: a null-conditional call into an OWNING collection's
// `Add(owned item: T)` carries the call-site `owned` marker; §22.3 governs
// that transfer, not §7, so the modifier guard must keep skipping it on the
// `?.` path (no §7 diagnostic from this helper).
BEZEL_TEST(sema_collection_nullcond_arg_2594,
           nullcond_owning_add_owned_marker_is_not_a_section7_slot) {
    auto r = parseSemaAll(
        "module M; "
        "class R "
        "  public V: int; "
        "  public constructor(v: int) this.V := v; end constructor; "
        "end R; "
        "class C "
        "  public static method T(s: OwningArray<R>?) "
        "    s?.Add(move new R(1)); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}
