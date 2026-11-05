// Regression tests for #8103 and #8104 (the #7970 / #7710 / #7756 family):
// a user extension over a `FrozenArray<T>` receiver.
//
// #8103.  Language spec §26.5 rule 2 (v0.355): "If the receiver's type
// declares no instance member of that name, the compiler searches ... for
// extension blocks on `x`'s type, base types, or implemented interfaces".
// stdlib §4.19 (v0.355) declares `public class FrozenArray<T>:
// IEnumerable<T>`, so an `extension IEnumerable<int>` block is a candidate
// for a `FrozenArray<int>` receiver.  The §26.5 builtin-interface ancestor
// table had no `FrozenArray` row (it has one for `Array`), and the walk
// skipped the `builtinClassSym` stub a static factory result carries
// (`FrozenArray<int>.From(a)`), so the call reported "produced no value, and
// no dispatch arm emitted any code" (#2457).
//
// #8104.  Spec §26.5 rule 1 (v0.355): "If `x`'s declared type ... declares
// any instance member named `Foo` ... instance resolution is committed and
// extension blocks are never consulted for that name on that receiver."
// stdlib §4.19 declares `ToArray(): owned Array<T>`.  #7756 committed rule 1
// for the factory STUB shape; the ANNOTATED shape
// (`using var f: FrozenArray<int> := ...`, a declaration-less generic Named)
// was never committed, so the same-named extension `ToArray(): int` still
// interfered and the statement was silently dropped (#3079).
//
// ORACLE: exact stdout, under runAotChecked (ASan / leak checked).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}
// Unchecked: the issue's literal `f.ToArray().ToString()` spelling leaks its
// owned temporary (a separate, pre-existing disposal gap on the unannotated
// control too), so those two rows pin stdout only.
std::string outPlain(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}
}  // namespace

// ---- #8103 ------------------------------------------------------------------

// The issue's literal repro.
BEZEL_TEST(frozenarray_extension_dispatch_8103, ienumerable_ext_on_frozen_factory_local) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "extension IEnumerable<int>\n"
            "  public method Foo(): int => 7;\n"
            "end;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var a: Array<int> := [1, 2, 3];\n"
            "    using var f := FrozenArray<int>.From(a);\n"
            "    Console.WriteLine(f.Foo());\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "7\n");
}

// The annotated shape reaches the same ancestor.
BEZEL_TEST(frozenarray_extension_dispatch_8103, ienumerable_ext_on_frozen_annotated_local) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "extension IEnumerable<int>\n"
            "  public method Foo(): int => 7;\n"
            "end;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var a: Array<int> := [1, 2, 3];\n"
            "    using var f: FrozenArray<int> := FrozenArray<int>.From(a);\n"
            "    Console.WriteLine(f.Foo());\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "7\n");
}

// Control: the same extension on an `Array<int>` receiver.
BEZEL_TEST(frozenarray_extension_dispatch_8103, ienumerable_ext_on_array_control) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "extension IEnumerable<int>\n"
            "  public method Foo(): int => 7;\n"
            "end;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var a: Array<int> := [1, 2, 3];\n"
            "    Console.WriteLine(a.Foo());\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "7\n");
}

// A heap-element (string) FrozenArray, 50x, so a leak or a double release in
// the dispatch shows under the checked oracle.
BEZEL_TEST(frozenarray_extension_dispatch_8103, ienumerable_string_ext_on_frozen_loop) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "extension IEnumerable<string>\n"
            "  public method Tag(): string => \"t\";\n"
            "end;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    var total := 0;\n"
            "    for var i in 0..<50 do\n"
            "      using var a: Array<string> := [\"x\", \"y\"];\n"
            "      using var f := FrozenArray<string>.From(a);\n"
            "      total := total + f.Tag().Length;\n"
            "    end;\n"
            "    Console.WriteLine(total);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "50\n");
}

// ---- #8104 ------------------------------------------------------------------

// The issue's literal repro.  The declared `ToArray(): owned Array<T>` wins.
BEZEL_TEST(frozenarray_extension_dispatch_8103, toarray_ext_never_wins_on_frozen_annotated_local) {
    BEZEL_EXPECT_EQ(
        outPlain("module M;\n"
            "extension FrozenArray<int>\n"
            "  public method ToArray(): int => 98;\n"
            "end;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var a: Array<int> := [1, 2, 3];\n"
            "    using var f: FrozenArray<int> := FrozenArray<int>.From(a);\n"
            "    Console.WriteLine(f.ToArray().ToString());\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "System.Collections.Generic.Array<int>\n");
}

// The built-in result is the owned Array: its `Count` is usable.
BEZEL_TEST(frozenarray_extension_dispatch_8103, toarray_ext_never_wins_count_on_frozen_annotated_local) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "extension FrozenArray<int>\n"
            "  public method ToArray(): int => 98;\n"
            "end;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var a: Array<int> := [1, 2, 3];\n"
            "    using var f: FrozenArray<int> := FrozenArray<int>.From(a);\n"
            "    using var b := f.ToArray();\n"
            "    Console.WriteLine(b.Count);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "3\n");
}

// Control: an extension name the type does NOT declare still dispatches on
// the annotated receiver (rule 2).
BEZEL_TEST(frozenarray_extension_dispatch_8103, undeclared_ext_name_still_dispatches_on_frozen_annotated_local) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "extension FrozenArray<int>\n"
            "  public method ToArray(): int => 98;\n"
            "  public method Foo(): int => 5;\n"
            "end;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var a: Array<int> := [1, 2, 3];\n"
            "    using var f: FrozenArray<int> := FrozenArray<int>.From(a);\n"
            "    Console.WriteLine(f.Foo());\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "5\n");
}

// Control: the un-annotated shape (already correct, #7756).
BEZEL_TEST(frozenarray_extension_dispatch_8103, toarray_ext_never_wins_on_frozen_factory_local_control) {
    BEZEL_EXPECT_EQ(
        outPlain("module M;\n"
            "extension FrozenArray<int>\n"
            "  public method ToArray(): int => 98;\n"
            "end;\n"
            "class C\n"
            "  public static method Main(): int\n"
            "    using var a: Array<int> := [1, 2, 3];\n"
            "    using var f := FrozenArray<int>.From(a);\n"
            "    Console.WriteLine(f.ToArray().ToString());\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n"),
        "System.Collections.Generic.Array<int>\n");
}
