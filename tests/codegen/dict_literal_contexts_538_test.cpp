// Fix #538 — dictionary literal `["k":v, …]` in field-initializer and
// return-statement contexts causes heap corruption / SIGSEGV.
//
// Spec: stdlib §4.8 (Dictionary<K,V>) — a dictionary literal must produce a
// correctly-initialized Dictionary<K,V> regardless of the context in which it
// appears: local declaration (fixed by #534), field initializer (fixed here),
// and return statement (fixed here).
//
// Root cause (pre-fix):
//   field-init path  (LlvmBackend_Setup.cpp ~L2782):
//     The guard `cl && !cl->isDict` in emitInstanceInit's field-initializer
//     loop admitted ONLY non-dict collection literals into the
//     materializeHeapCollectionLiteral branch; a dict literal fell through to
//     the trailing `lowerExpr(*init)` which returned null for
//     CollectionLiteralExpr. The field slot was left null (calloc-zeroed),
//     giving a Count read of 0 and a crash on any bz_dict_* call.
//
//   return-statement path (LlvmBackend_Stmts.cpp ~L328):
//     The guard `!cl->isDict` in the return-statement's collection-literal
//     fast path was identical: only non-dict literals went through
//     materializeHeapCollectionLiteral; a dict literal fell through to
//     `lowerExpr(*rs->value)` which returned null, and the caller received a
//     null pointer as the "dictionary" — any Count/TryGetValue on it crashed.
//
// Fix: in each site, add a parallel branch for `cl->isDict && !cl->keys.empty()`
// that calls the existing `materializeHeapDictLiteral(cl, dictTy)` helper
// introduced by #534, with the resolved field type / return type as `dictTy`.
// The helper already handles both string-key and generic-key tables.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// Field-initializer context
// A class field declared as `private _d: Dictionary<string,int> := ["a":1,"b":2];`
// must be populated by emitInstanceInit (called from `new C()`) via
// materializeHeapDictLiteral, not left null by the old fall-through.
// ---------------------------------------------------------------------------

// #538 / stdlib §4.8: dict-literal field initializer — Count and value correctness.
BEZEL_TEST(dict_literal_contexts_538, field_init_string_key_count_and_lookup) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  private _d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "  public method CountAndA(): int "
        "    var va: int? := null; "  // v0.171 (#1336): out is V?
        "    _d.TryGetValue(\"a\", out va); "
        "    return int(_d.Count = 2 and va = 1); "
        "  end CountAndA; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.CountAndA(); "
        "  end Main; "
        "end C;"), 1);
}

// #538 / stdlib §4.8: dict-literal field initializer — both values readable.
BEZEL_TEST(dict_literal_contexts_538, field_init_string_key_both_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  private _d: Dictionary<string, int> := [\"x\": 10, \"y\": 20]; "
        "  public method GetX(): int "
        "    var v: int? := null; "  // v0.171 (#1336): out is V?
        "    _d.TryGetValue(\"x\", out v); "
        "    return v!; "
        "  end GetX; "
        "  public method GetY(): int "
        "    var v: int? := null; "  // v0.171 (#1336): out is V?
        "    _d.TryGetValue(\"y\", out v); "
        "    return v!; "
        "  end GetY; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return int(c.GetX() = 10 and c.GetY() = 20); "
        "  end Main; "
        "end C;"), 1);
}

// #538 / stdlib §4.8: dict-literal field initializer — three-entry literal.
BEZEL_TEST(dict_literal_contexts_538, field_init_three_entries) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  private _d: Dictionary<string, int> := [\"one\": 1, \"two\": 2, \"three\": 3]; "
        "  public method Check(): int "
        "    var v1: int? := null; var v2: int? := null; var v3: int? := null; "  // v0.171 (#1336)
        "    _d.TryGetValue(\"one\",   out v1); "
        "    _d.TryGetValue(\"two\",   out v2); "
        "    _d.TryGetValue(\"three\", out v3); "
        "    return int(_d.Count = 3 and v1 = 1 and v2 = 2 and v3 = 3); "
        "  end Check; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Check(); "
        "  end Main; "
        "end C;"), 1);
}

// ---------------------------------------------------------------------------
// Return-statement context
// A method returning `owned Dictionary<string,int>` with body
// `return ["x": 9];` must produce a properly-allocated dict, not a null pointer.
// ---------------------------------------------------------------------------

// #538 / stdlib §4.8: dict literal in return — Count==1, correct value.
BEZEL_TEST(dict_literal_contexts_538, return_single_entry_count_and_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Make(): owned Dictionary<string, int> "
        "    return [\"x\": 9]; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var d := C.Make(); "
        "    var v: int? := null; "  // v0.171 (#1336): out is V?
        "    d.TryGetValue(\"x\", out v); "
        "    return int(d.Count = 1 and v = 9); "
        "  end Main; "
        "end C;"), 1);
}

// #538 / stdlib §4.8: dict literal in return — two entries, both readable.
BEZEL_TEST(dict_literal_contexts_538, return_two_entries) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Make(): owned Dictionary<string, int> "
        "    return [\"a\": 1, \"b\": 2]; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var d := C.Make(); "
        "    var va: int? := null; var vb: int? := null; "  // v0.171 (#1336)
        "    d.TryGetValue(\"a\", out va); "
        "    d.TryGetValue(\"b\", out vb); "
        "    return int(d.Count = 2 and va = 1 and vb = 2); "
        "  end Main; "
        "end C;"), 1);
}

// #538 / stdlib §4.8: dict literal in return — ContainsKey correct.
BEZEL_TEST(dict_literal_contexts_538, return_contains_key) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Make(): owned Dictionary<string, int> "
        "    return [\"p\": 7, \"q\": 8]; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var d := C.Make(); "
        "    return int(d.ContainsKey(\"p\") and d.ContainsKey(\"q\") and not d.ContainsKey(\"r\")); "
        "  end Main; "
        "end C;"), 1);
}

// ---------------------------------------------------------------------------
// Control cases — array/set literal as field-init + return still work;
// #534 local-decl dict literal still works; new()+Add baseline still works.
// ---------------------------------------------------------------------------

// Control: Array literal field initializer is unaffected.
BEZEL_TEST(dict_literal_contexts_538, control_array_field_init_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  private _a: Array<int> := [10, 20, 30]; "
        "  public method Check(): int "
        "    return int(_a.Count = 3 and _a.Get(0) = 10 and _a.Get(2) = 30); "
        "  end Check; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Check(); "
        "  end Main; "
        "end C;"), 1);
}

// Control: Array literal return is unaffected.
BEZEL_TEST(dict_literal_contexts_538, control_array_return_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Make(): owned Array<int> "
        "    return [5, 6, 7]; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var a := C.Make(); "
        "    return int(a.Count = 3 and a.Get(0) = 5 and a.Get(2) = 7); "
        "  end Main; "
        "end C;"), 1);
}

// Control: #534 local-decl dict literal still works.
BEZEL_TEST(dict_literal_contexts_538, control_local_dict_literal_534_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "  var va: int? := null; var vb: int? := null; "  // v0.171 (#1336)
        "  d.TryGetValue(\"a\", out va); "
        "  d.TryGetValue(\"b\", out vb); "
        "  return int(d.Count = 2 and va = 1 and vb = 2); "
        "end Main; end C;"), 1);
}

// Control: new()+Add baseline is unaffected.
BEZEL_TEST(dict_literal_contexts_538, control_new_plus_add_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"x\", 9); "
        "  var v: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(\"x\", out v); "
        "  return int(d.Count = 1 and v = 9); "
        "end Main; end C;"), 1);
}
