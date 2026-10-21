// fix-forward #6776 (spec §6.2, §28.3, §28.8, §2.5; stdlib §4.18; v0.340):
// scope-end disposal of a builtin whose TypeRef comes over the IMPORT wire.
//
// An imported factory's `owned Mutex` / `owned SortedSet<int>` return type has
// no `astNode` in the importing module (the name travels on `importedName`;
// `TypeRef::headName()` reads both). Two disposal sites misread such a value:
//
//  (a) the `using var` Mutex arm (LlvmBackend_Setup.cpp, "§6.2 / issue #647")
//      gated on `rt->astNode`, so `using var x := Lib.MX();` registered NO
//      disposal and the Mutex leaked at scope end. MEASURED at 4bf709ff
//      (pre-#6776) AND 78d28fd2: 100 iterations leak 100 Mutexes (4,800 B /
//      100 allocs, then 8,800 B / 200 once #6776 tagged the Mutex). The
//      same-module `new Mutex()` and the annotated `using var x: Mutex := …`
//      spellings are clean on the same binaries. The #6776 regression matrix
//      surfaced it as `using var x := Lib.MX(); H.Show(x);` (an Object-typed
//      borrow): CLEAN at 4bf709ff only because the untagged Mutex's Object
//      render freed it by accident (a second `H.Show(x)` SEGVs there), LEAK at
//      78d28fd2 where the render is correct.
//  (b) `buildCollectionTempDisposal`'s set-family test named `Set` and
//      `OwningSet` but not `SortedSet`, so the §28.8 owned-parameter auto-drop
//      freed an imported (or local) `SortedSet<int>` table through
//      bz_array_free: SEGV at 78d28fd2 (the moved bz_array_t layout), a leak
//      before. The same-module twin is pinned in
//      tests/codegen/dict_backed_set_drop_on_overwrite_6776_test.cpp.
//
// Every row runs under `runAotModulesChecked` (generated-code ASan + LSan,
// exitcode 99 on a leak), looped 100×, and pins the printed answer too.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

constexpr const char* kLib6776b =
    "module Lib6776b; "
    "import System.Threading; "
    "public class M6776b "
    "  public static method MX(): owned Mutex "
    "    var v := new Mutex(); "
    "    return v; "
    "  end MX; "
    "  public static method SS(): owned SortedSet<int> "
    "    var v := new SortedSet<int>(); v.Add(7); v.Add(5); v.Add(6); "
    "    return v; "
    "  end SS; "
    "end M6776b;";

// The importer: `helpers` are extra class members of H6776b, `body` is the
// loop body (100 iterations).
std::string app6776b(const std::string& helpers, const std::string& body) {
    return "module App6776b; "
           "import Lib6776b; "
           "import System.Threading; "
           "class H6776b " + helpers + " end H6776b; "
           "class C6776b public static method Main(): int "
           "  var i := 0; "
           "  while i < 100 do "
           "    " + body + " "
           "    i += 1; "
           "  end; "
           "  return 0; "
           "end Main; end C6776b;";
}

std::string rep(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

test::JitResult runChecked6776b(const std::string& helpers,
                                const std::string& body) {
    return test::runAotModulesChecked(
        {{"Lib6776b", kLib6776b}, {"App6776b", app6776b(helpers, body)}},
        "App6776b");
}

}  // namespace

// §6.2 / §28.3: a `using var` Mutex is disposed at scope end. The type is
// INFERRED from the imported factory. Leak → exitcode 99.
BEZEL_TEST(imported_builtin_scope_end_disposal_6776,
           imported_mutex_using_var_inferred_type_is_disposed_checked_spec_6_2_28_3) {
    auto r = runChecked6776b(
        "",
        "using var x := M6776b.MX(); using var lk := x.Acquire();"
        " Console.WriteLine(\"m\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("m\n", 100));
}

// The matrix cell: the same `using var`, borrowed through an Object-typed
// parameter (§8.2). Pinned WITHOUT rendering the Object so the row asserts
// the disposal only (the §8.5 render is pinned by
// object_param_vtableless_builtin_6776_test.cpp).
BEZEL_TEST(imported_builtin_scope_end_disposal_6776,
           imported_mutex_using_var_borrowed_as_object_is_disposed_checked_spec_8_2_28_3) {
    auto r = runChecked6776b(
        "public static method Show(o: Object) Console.WriteLine(\"o\"); end Show;",
        "using var x := M6776b.MX(); H6776b.Show(x); H6776b.Show(x);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("o\no\n", 100));
}

// Control: the annotated spelling was clean before the fix and stays clean.
BEZEL_TEST(imported_builtin_scope_end_disposal_6776,
           control_imported_mutex_using_var_annotated_type_is_disposed_checked_spec_6_2) {
    auto r = runChecked6776b(
        "",
        "using var x: Mutex := M6776b.MX(); using var lk := x.Acquire();"
        " Console.WriteLine(\"m\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("m\n", 100));
}

// §28.8 / stdlib §4.18: an `owned v: SortedSet<int>` parameter is auto-disposed
// at the callee's scope end — through bz_dict_free, as the table is hash-backed.
BEZEL_TEST(imported_builtin_scope_end_disposal_6776,
           imported_sortedset_owned_parameter_auto_dispose_checked_spec_28_8_4_18) {
    auto r = runChecked6776b(
        "public static method Sink(owned v: SortedSet<int>)"
        " Console.WriteLine($\"{v.Count} {v.Contains(6)} {v.First()}\"); end Sink;",
        "var x := M6776b.SS(); H6776b.Sink(move x);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, rep("3 True 5\n", 100));
}

}  // namespace bezel::module
