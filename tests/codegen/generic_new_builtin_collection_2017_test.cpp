// Codegen — #2017 (§14.2, v0.197): `new T()` inside a `where T: new()` generic
// body, instantiated with a BUILTIN-COLLECTION type argument (`T = Array<int>`,
// `T = Dictionary<K,V>`), SEGV'd at runtime.
//
// SPEC: §14.2 (v0.197) — a class substitution satisfies `where T: new()` when it
// declares a public parameterless constructor; §14.2 explicitly lists `Array<T>`
// among the class types, and stdlib §4.4 declares `Array<T>` a `public class`
// with a `public constructor();`. So a builtin collection DOES satisfy `new()`,
// and `new T()` must construct a real, empty collection (this is a CODEGEN
// obligation, not a sema rejection).
//
// ROOT CAUSE: in lowerNew, the bare-`T` -> concrete-substitution resolution only
// fired when the substitution carried a user TypeSymbol (`sit->second->
// declaration != null`). A builtin collection has NO declaration, so `rt` stayed
// the bare parameter `T`, the stdlib-collection construction dispatch keyed off a
// `tnode` still named `T` (`colName == "T"`), every branch fell through, and
// lowerNew returned nullptr — the local slot was never initialized. A later use
// (`Add`) dereferenced garbage -> SIGSEGV. The fix resolves `rt` for a
// no-declaration Named substitution too and redirects `tnode` to the resolved
// collection's node.
//
// A leak / double-free / use-after-free / SEGV flips r.ran to false -> -999,
// distinct from any legal exit value; runAotChecked forces generated-code ASan.

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The `where T: new()` factory whose body constructs `new T()` and returns it as
// `owned T` — the exact shape of the issue's repro.
const char* kFactory =
    "static class Factory "
    "  public static method Make<T>(): owned T where T: new() "
    "    var x := new T(); "
    "    return x; "
    "  end Make; "
    "end Factory; ";
}  // namespace

// #2017 (§14.2, v0.197): the canonical repro. `new T()` with `T = Array<int>`
// must construct a real, usable, empty `Array<int>`. Add two elements, read
// Count. Before the fix: `new T()` returned nullptr, the `using var` slot held
// garbage, and `Add` SEGV'd (-999). With the fix: Count == 2.
BEZEL_TEST(codegen_generic_new_builtin_collection_2017,
           new_t_with_array_int_constructs_usable_array_spec_14_2) {
    std::string src =
        std::string("module M; ") + kFactory +
        "class C public static method Main(): int "
        "  using var a := Factory.Make<Array<int>>(); "
        "  a.Add(7); "
        "  a.Add(9); "
        "  return a.Count; "                  // 2
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

// #2017 (§14.2, v0.197): a second builtin collection under the same constraint —
// `T = Dictionary<string, int>`. Two inserts, read Count. Before the fix: the
// dictionary slot was uninitialized -> SEGV on the first `[]:=` (-999). With the
// fix: Count == 2.
BEZEL_TEST(codegen_generic_new_builtin_collection_2017,
           new_t_with_dictionary_constructs_usable_dict_spec_14_2) {
    std::string src =
        std::string("module M; ") + kFactory +
        "class C public static method Main(): int "
        "  using var d := Factory.Make<Dictionary<string, int>>(); "
        "  d[\"a\"] := 10; "
        "  d[\"b\"] := 20; "
        "  return d.Count; "                  // 2
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

// #2017 sibling regression (§14.2): the legitimate class case must NOT regress.
// `new T()` with `T = Widget` (a user class with a public parameterless ctor)
// still constructs via the class constructor and disposes at scope end. Widget.n
// (the live count) returns to 0 after the `do` block. This path is unchanged by
// the fix (a class substitution carries a declaration and skips the builtin
// dispatch), and the test pins that it stays working.
BEZEL_TEST(codegen_generic_new_builtin_collection_2017,
           new_t_with_user_class_still_constructs_and_disposes_spec_14_2) {
    std::string src =
        std::string("module M; ") +
        "class Widget "
        "  public static n: int := 0; "
        "  public constructor() Widget.n := Widget.n + 1; end constructor; "
        "  destructor Widget.n := Widget.n - 1; end destructor; "
        "end Widget; " +
        kFactory +
        "class C public static method Main(): int "
        "  do "
        "    using var w := Factory.Make<Widget>(); "
        "  end; "
        "  return Widget.n; "                 // 0
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}
