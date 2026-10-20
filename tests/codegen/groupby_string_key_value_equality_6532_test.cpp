// Codegen / GroupBy key equality — issue #6532. `GroupBy` with a string key
// selector did not merge equal keys: ["aa","bb","aa"] yielded three groups.
// Spec: stdlib §5 GroupBy (`where TKey: hashable`; groups in first-occurrence-
// of-key order, one group per distinct key by the key type's value equality,
// Bezel_Standard_Library.md, spec v0.339); language spec §8.3 (hashable
// equality is value equality for string and record).
//
// THE BUG. bz_groupby_to_array compared keys with memcmp over the key bytes.
// A string key is a pointer, so two equal strings at different addresses (a
// concatenation, an interpolation, a record field) never matched. The fix
// passes the key type's value-equality trampoline (the one Distinct uses,
// buildLinqTrampoline kind 3) and the runtime compares through it.
//
// Oracle: runAotChecked (ASan/LSan) with stdout pinned byte-for-byte.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

const char* kProg =
    "module M; "
    "import System.Collections.Linq; "
    "record Person(name: string, age: int); "
    "record Pt(x: int, y: int); "
    "class C "
    "  public static method Main(): int "
    // literals: pointer-equal in practice (control for the original repro)
    "    using var a := new Array<string>(); "
    "    a.Add(\"aa\"); a.Add(\"bb\"); a.Add(\"aa\"); "
    "    for var g in a.GroupBy(lambda(s) => s) do "
    "      Console.WriteLine(\"lit \" + g.Key + \"=\" + g.Count().ToString()); "
    "    end; "
    "    using var n := new Array<int>(); "
    "    n.Add(1); n.Add(2); n.Add(1); n.Add(3); n.Add(2); n.Add(1); "
    // controls: int key and record key
    "    for var g in n.GroupBy(lambda(i) => i mod 2) do "
    "      Console.WriteLine(\"mod \" + g.Key.ToString() + \"=\" + g.Count().ToString()); "
    "    end; "
    "    using var pts := new Array<Pt>(); "
    "    pts.Add(new Pt(1, 2)); pts.Add(new Pt(3, 4)); pts.Add(new Pt(1, 2)); "
    "    for var g in pts.GroupBy(lambda(q: Pt) => q) do "
    "      Console.WriteLine(\"pt \" + g.Key.x.ToString() + \"=\" + g.Count().ToString()); "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// Fresh keys (concatenation, interpolation, a record field of concatenated
// strings): equal values at distinct pointers, the actual #6532 condition.
// runAot, not runAotChecked: a GroupBy over heap-string elements leaks today
// at baseline (separate follow-up), so LSan would fail these for another reason.
const char* kFresh =
    "module M; "
    "import System.Collections.Linq; "
    "record Person(name: string, age: int); "
    "class C "
    "  public static method Main(): int "
    // equal values, distinct pointers: built by concatenation
    "    using var b := new Array<string>(); "
    "    var p := \"a\"; var q := \"b\"; "
    "    b.Add(p + \"a\"); b.Add(q + \"b\"); b.Add(p + \"a\"); b.Add(q + q); "
    "    for var g in b.GroupBy(lambda(s) => s) do "
    "      Console.WriteLine(\"cat \" + g.Key + \"=\" + g.Count().ToString()); "
    "    end; "
    "    using var n := new Array<int>(); "
    "    n.Add(1); n.Add(2); n.Add(1); n.Add(3); n.Add(2); n.Add(1); "
    "    for var g in n.GroupBy(lambda(i) => $\"k{i}\") do "
    "      Console.WriteLine(\"int \" + g.Key + \"=\" + g.Count().ToString()); "
    "    end; "
    // selector over records returning a string field
    "    using var ps := new Array<Person>(); "
    "    ps.Add(new Person(b[0], 1)); ps.Add(new Person(b[1], 2)); "
    "    ps.Add(new Person(b[2], 3)); "
    "    for var g in ps.GroupBy(lambda(q: Person) => q.name) do "
    "      Console.WriteLine(\"rec \" + g.Key + \"=\" + g.Count().ToString()); "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

BEZEL_TEST(groupby_string_key_value_equality_6532,
           literal_string_int_record_keys_merge_lsan_clean_stdlib_5) {
    JitResult r = runAotChecked(kProg);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "lit aa=2\nlit bb=1\n"
        "mod 1=4\nmod 0=2\n"
        "pt 1=2\npt 3=1\n");
}

BEZEL_TEST(groupby_string_key_value_equality_6532,
           fresh_string_keys_merge_by_value_stdlib_5) {
    JitResult r = runAot(kFresh);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "cat aa=2\ncat bb=2\n"
        "int k1=3\nint k2=2\nint k3=1\n"
        "rec aa=2\nrec bb=1\n");
}
