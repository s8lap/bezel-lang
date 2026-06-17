// fix #774 / §23.4: indexers may be overloaded by parameter type list, and a
// call dispatches to the correct overload by the argument's type.
//
// §23.4: "Indexers may be overloaded by parameter type list." Two indexers with
// distinct parameter types (`[i: int]` and `[b: bool]`) are a legal overload
// pair. Before the fix the declaration was rejected as a "duplicate member
// `.this`" (FALSE_REJECT), because indexer PropertySymbols were not treated as
// overloadable; and the codegen/sema indexer lookup returned the FIRST indexer
// regardless of argument type, so dispatch and result typing were wrong.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
bool rejected(const std::string& program) {
    return !runJit(program).ran;
}
}  // namespace

// §23.4: an `[i: int]` and a `[b: bool]` overload (same return type) — the call
// dispatches by argument type: m[5] -> "int", m[true] -> "bool". (The repro.)
BEZEL_TEST(indexer_overload_774, dispatch_same_return_type) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class MultiIndex "
        "  public property [i: int]: string => \"int\"; "
        "  public property [b: bool]: string => \"bool\"; "
        "end MultiIndex; "
        "class C public static method Main(): int "
        "  using var m := new MultiIndex(); "
        "  return 0 if m[5] = \"int\" and m[true] = \"bool\" else 1; "
        "end Main; end C;"), 0);
}

// §23.4: overloads with DIFFERENT return types — the int overload yields int,
// the bool overload yields string. Exercises both codegen dispatch (right
// getter) and sema typing (right IndexExpr result type). m[5] -> 50 (int),
// m[true] -> "yes" (string).
BEZEL_TEST(indexer_overload_774, dispatch_different_return_types) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class MultiIndex "
        "  public property [i: int]: int => i * 10; "
        "  public property [b: bool]: string => \"yes\" if b else \"no\"; "
        "end MultiIndex; "
        "class C public static method Main(): int "
        "  using var m := new MultiIndex(); "
        "  var n := m[5]; "
        "  var s := m[true]; "
        "  return 0 if n = 50 and s = \"yes\" else 1; "
        "end Main; end C;"), 0);
}

// §23.4 regression guard: a class with a SINGLE indexer is unaffected by the
// overload-dispatch path (it falls back to the only indexer). b[5] -> 105.
BEZEL_TEST(indexer_overload_774, single_indexer_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box public property [i: int]: int => i + 100; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  return 0 if b[5] = 105 else 1; "
        "end Main; end C;"), 0);
}

// §22.6 / §23.4: two indexers with the SAME parameter type are still an
// indistinguishable-overload error (the distinctness check now covers indexers).
BEZEL_TEST(indexer_overload_774, duplicate_param_type_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "class Dup "
        "  public property [i: int]: int => i; "
        "  public property [j: int]: int => j + 1; "
        "end Dup; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}
