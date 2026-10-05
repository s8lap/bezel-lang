// Spec-derived regression test for issue #2582.
//
// §7 (Nullability) / stdlib §4.4–§4.18 (collections), v0.236: a builtin
// collection instance method that declares a NON-nullable `item: T` /
// `query: T` / `key: K` parameter must reject an UN-narrowed `T?` (or `K?`)
// argument at compile time — the same §7 assignability rule a user-defined
// `method Take(x: int)` enforces. Previously the builtin collection call path
// skipped the per-argument assignability check (no MethodSymbol), silently
// accepting the un-narrowed nullable and reaching codegen, where the equality-
// compare lowering tripped an LLVM ICmpInst assertion (`ICmpInst::AssertOK`:
// operand-type mismatch, exit 134). This is the collection twin of #2315
// (builtin string methods accepting un-narrowed `string?`).
//
// This is a REJECTION fix, so each negative case asserts on diagnostic TEXT
// (not the coarse rejects() helper — runAot's WIFSIGNALED path synthesizes an
// Error diagnostic for a runtime SIGSEGV/ICE too, so it cannot distinguish a
// genuine reject from the crash). The positive cases guard against false
// rejection of working code.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
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

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §12/§7 "nullable -> non-nullable" assignability diagnostic fragment
// shared by the user-method path and (after #2582) the builtin-collection path.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2582 / §7 / stdlib §4.4: the issue's repro — `arr.IndexOf(n)` with an
// un-narrowed `int?` on `Array<int>`. Without the fix this ICE'd codegen
// (LLVM ICmpInst assertion, exit 134); with the fix it is a clean sema reject.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_array_indexof_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var arr := new Array<int>(1); "
        "    arr[0] := 7; "
        "    return arr.IndexOf(N()); "  // N(): int? — un-narrowed
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.IndexOf"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2582 / stdlib §4.4: `arr.Contains(n)` rejects the same un-narrowed `int?`.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_array_contains_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): bool "
        "    var arr := new Array<int>(1); "
        "    arr[0] := 7; "
        "    return arr.Contains(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.Contains"));
}

// #2582 / stdlib §4.4: `arr.LastIndexOf(n)` (v0.228) — the rest of the
// Array search family.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_array_lastindexof_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var arr := new Array<int>(1); "
        "    arr[0] := 7; "
        "    return arr.LastIndexOf(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.LastIndexOf"));
}

// #2582 / stdlib §4.4: `arr.Remove(n)` (the `Remove(item: T): bool` form, NOT
// RemoveAt(index)) rejects the un-narrowed `int?`.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_array_remove_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): bool "
        "    var arr := new Array<int>(1); "
        "    arr[0] := 7; "
        "    return arr.Remove(N()); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.Remove"));
}

// #2582 / stdlib §4.4: `arr.Insert(0, n)` — the item sits at parameter
// position 1 (the index at position 0 is a plain int).
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_array_insert_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var arr := new Array<int>(1); "
        "    arr.Insert(0, N()); "  // item: int <- N(): int?
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Array<T>.Insert"));
}

// #2582 / stdlib §4.11: the Stack/Queue family — `s.Push(n)` and `q.Enqueue(n)`
// declare `item: T` and reject the un-narrowed `int?`.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_stack_queue_reject_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var coll := new {TYPE}<int>(); "
        "    coll.{METHOD}(N()); "
        "    return 0; "
        "  end T; "
        "end C;";
    struct Case { const char* type; const char* method; };
    for (const auto& c : std::initializer_list<Case>{
            {"Stack", "Push"},
            {"Queue", "Enqueue"},
            {"Stack", "Contains"},
            {"Queue", "Contains"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{TYPE}"), 6, c.type);
        src.replace(src.find("{METHOD}"), 8, c.method);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    }
}

// #2582 / stdlib §4.10: `set.Add(n)` / `set.TryAdd(n)` / `set.Contains(n)` /
// `set.Remove(n)` on `Set<int>` all reject the un-narrowed `int?`.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_set_rejects_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): {RET} "
        "    var s := new Set<int>(); "
        "    return s.{METHOD}(N()); "
        "  end T; "
        "end C;";
    struct Case { const char* method; const char* ret; };
    for (const auto& c : std::initializer_list<Case>{
            {"Add",      "()"},
            {"TryAdd",   "bool"},
            {"Contains", "bool"},
            {"Remove",   "bool"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{RET}"), 5, c.ret);
        src.replace(src.find("{METHOD}"), 8, c.method);
        // `Add` returns () — wrap so T returns int regardless.
        if (c.method == std::string("Add")) {
            src = std::string(
                "module M; class C "
                "  private static method N(): int? => null; "
                "  public static method T(): int "
                "    var s := new Set<int>(); "
                "    s.Add(N()); "
                "    return 0; "
                "  end T; end C;");
        }
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
        std::string meth = std::string("Set<T>.") + c.method;
        BEZEL_EXPECT_TRUE(hasError(r, meth.c_str()));
    }
}

// #2582 / stdlib §4.8: `dict.ContainsKey(n)` / `dict.Contains(n)` /
// `dict.Remove(n)` on `Dictionary<int, int>` reject the un-narrowed `int?` KEY
// (K = genericArgs[0]). `dict.Add(k, v)` also checks the key (pos 0); the value
// at pos 1 is a separate §7 slot NOT covered by this fix (filed separately).
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_dictionary_key_rejects_unnarrowed_nullable) {
    const char* tmpl =
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): {RET} "
        "    var d := new Dictionary<int, int>(); "
        "    return d.{METHOD}(N()); "
        "  end T; "
        "end C;";
    struct Case { const char* method; const char* ret; };
    for (const auto& c : std::initializer_list<Case>{
            {"ContainsKey", "bool"},
            {"Contains",    "bool"},
            {"Remove",      "bool"},
        }) {
        std::string src = tmpl;
        src.replace(src.find("{RET}"), 5, c.ret);
        src.replace(src.find("{METHOD}"), 8, c.method);
        auto r = parseSemaAll(src);
        BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    }
}

// #2582 / stdlib §4.8: `dict.Add(k, v)` — the two-arg Add form. The KEY at
// position 0 (K = genericArgs[0]) is checked; an un-narrowed `int?` key is
// rejected. (The VALUE at position 1 is a separate §7 slot of type V —
// genericArgs[1] — NOT covered by this fix; filed separately.)
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_dictionary_add_rejects_unnarrowed_nullable_key) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var d := new Dictionary<int, string>(); "
        "    d.Add(N(), \"x\"); "  // key: int <- N(): int? (pos 0)
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Dictionary<T>.Add"));
}

// #2582 / stdlib §4.5: `oarr.IndexOf(n)` on `OwningArray<int>` — the OWNING
// family's borrow-taking query methods are covered too. The owning `Add(owned
// item: T)` mutator carries a §22.3 `owned` modifier and is governed by §22.3,
// not §7 — it is not asserted here (the modifier path is its own rule).
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_owningarray_indexof_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var arr := new OwningArray<int>(); "
        "    return arr.IndexOf(N()); "  // OwningArray<T>.IndexOf(item: T): int
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "OwningArray<T>.IndexOf"));
}

// #2582 / §7.4: a NARROWED `int?` (after a null-check guard) IS assignable to
// the non-nullable `int` parameter — only the un-narrowed form is the hole.
// This positive case must compile with NO nullable-assignability error (guards
// against a false-reject regression in the narrowing-aware path).
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_indexof_accepts_narrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var arr := new Array<int>(1); "
        "    arr[0] := 7; "
        "    var n: int? := 5; "
        "    if n = null then return -1; end; "  // narrows n non-null below
        "    return arr.IndexOf(n); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2582 / std7: a plain non-nullable `int` argument compiles unchanged
// (positive baseline — the fix must not reject working code).
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_indexof_accepts_plain_int) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var arr := new Array<int>(1); "
        "    arr[0] := 7; "
        "    var k: int := 5; "
        "    return arr.IndexOf(k); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2582 / §7: when the collection's element type IS itself nullable —
// `Array<int?>` declares `IndexOf(item: int?)` — an `int?` argument is
// admissible (the parameter's nullability is the instantiated type's
// nullability). Guards against a false-reject where the helper would force
// non-nullability regardless of T.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_indexof_accepts_nullable_element_collection) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var arr := new Array<int?>(1); "
        "    return arr.IndexOf(N()); "  // Array<int?>.IndexOf(item: int?) — OK
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2582 / stdlib §4.4: the integer second parameter of `IndexOf(item,
// startIndex)` is NOT an item slot — a plain int startIndex compiles, and the
// check must not mis-fire on it (guards against a position-classification
// regression on the 2-arg overload).
BEZEL_TEST(sema_collection_nullable_arg_2582,
           builtin_indexof_two_arg_overload_accepts_int_startindex) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var arr := new Array<int>(2); "
        "    arr[0] := 7; "
        "    arr[1] := 9; "
        "    return arr.IndexOf(9, 0); "  // (item: int, startIndex: int)
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2582 / stdlib §5 (LINQ): `arr.Take(count: int)` on `Array<T>` is the LINQ
// slicing operator — its `int` parameter is NOT an item slot. The fix MUST NOT
// mis-classify Take as an item-query on the non-OwningSet families (on
// OwningSet, Take IS the item-query `Take(query: T): owned T`; sema's existing
// resolution splits the two by family at L4363, and this check mirrors that
// split). Without the family gate this test false-rejected `arr.Take(2)` as
// `int`-not-assignable-to-`string` on `Array<string>`.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           linq_take_count_not_misclassified_as_item_query) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(): int "
        "    var arr := new Array<string>(3); "
        "    arr[0] := \"a\"; arr[1] := \"b\"; arr[2] := \"c\"; "
        "    var first2 := arr.Take(2); "  // LINQ Take(count: int) — not an item
        "    return first2.Count(); "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// fix #5362 / stdlib §4.10 (v0.259): `OwningSet<T>.Take` was renamed to
// `Pop`. The pre-rename `Take` name is no longer on the stdlib surface,
// so `s.Take(N())` on an OwningSet is rejected as "no member `Take` on
// `OwningSet<T>`" — the owning-container unknown-member check fires before
// any nullable-argument check would run.
BEZEL_TEST(sema_collection_nullable_arg_2582,
           owningset_take_rejected_as_unknown_member) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(): int "
        "    var s := new OwningSet<int>(); "
        "    var t := s.Take(N()); "  // OwningSet<T>.Take — no longer on stdlib surface
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "no member `Take` on `OwningSet"));
}
