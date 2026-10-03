// fix #5339 (§14.1 "Generic declarations" / §23.4 "Indexers", v0.313): a
// block-bodied indexer getter (`get => do ... end;`) on a generic class is a
// standalone function. `ensureIndexerBlockGetter` memoized it per (PropertyDecl,
// accessor tag) and a DIRECT generic receiver (`b: Holder<string>`) installed no
// §14.1 substitution at the read call site (`lowerIndexerGet`), so the getter was built
// against the erased T->i32 template and that first build served every later
// instantiation. The sibling of #5315 (property block getter, c8320f96) and of
// #4973 (the derived-receiver case). The memo is now keyed on the owner's
// instantiation (`propertyThunkSubstKey`) and the receiver's substitution is
// installed for the direct case. NARROW: the COMPOUND site (`a[i] += x`) is not
// covered; its inlined setter half has the same missing substitution.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Issue repro A: int-typed getter reading a sibling field after the T field.
constexpr const char* k_int_sibling =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property [i: int]: int get => do return _w + i; end; end;\n"
    "  public constructor(x: T, y: int) _v := x; _w := y; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    Console.WriteLine(hi[1]);\n"
    "    Console.WriteLine(hs[1]);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Issue repro B: T-typed getter; three instantiations incl. a class-typed T.
constexpr const char* k_t_typed =
    "module M;\n"
    "class Pt\n"
    "  public property X: int;\n"
    "  public constructor(x: int) X := x; end constructor;\n"
    "end Pt;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property [i: int]: T get => do return _v; end; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hp := new Holder<Pt>(new Pt(7));\n"
    "    Console.WriteLine(hs[1]);\n"
    "    Console.WriteLine(hi[1]);\n"
    "    Console.WriteLine(hp[1].X);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Through a method of the generic class, a derived `D : Holder<int>` (control:
// the #4973 ancestor path), a loop (200x), and one same-instantiation repeat.
constexpr const char* k_via_method_derived_loop =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property [i: int]: int get => do return _w + i; end; end;\n"
    "  public constructor(x: T, y: int) _v := x; _w := y; end constructor;\n"
    "  public method Via(i: int): int return this[i]; end Via;\n"
    "end Holder;\n"
    "class D : Holder<int>\n"
    "  public constructor() base(1, 5); end constructor;\n"
    "end D;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var d := new D();\n"
    "    var sum := 0;\n"
    "    var k := 0;\n"
    "    while k < 200 do\n"
    "      sum := sum + hs[k] + hi[k] + hs.Via(k) + hi.Via(k) + d[k];\n"
    "      k := k + 1;\n"
    "    end;\n"
    "    Console.WriteLine(sum);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Control: a non-generic owner, and a single instantiation.
constexpr const char* k_control =
    "module M;\n"
    "class Plain\n"
    "  public _w: int;\n"
    "  public property [i: int]: int get => do return _w + i; end; end;\n"
    "  public constructor(y: int) _w := y; end constructor;\n"
    "end Plain;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property [i: int]: int get => do return _w + i; end; end;\n"
    "  public constructor(x: T, y: int) _v := x; _w := y; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var p := new Plain(4);\n"
    "    var a := new Holder<int>(3, 11);\n"
    "    var b := new Holder<int>(5, 30);\n"
    "    Console.WriteLine(p[1]);\n"
    "    Console.WriteLine(a[1]);\n"
    "    Console.WriteLine(b[2]);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// FIX ROW. FAIL-WITHOUT: `12` then a garbage value.
BEZEL_TEST(generic_indexer_block_getter_per_instantiation_5339,
           issue_5339_int_sibling_two_instantiations) {
    JitResult r = runAot(k_int_sibling);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12\n23\n"));
    JitResult c = runAotChecked(k_int_sibling);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("12\n23\n"));
}

// FIX ROW. FAIL-WITHOUT: `0` for the string read.
BEZEL_TEST(generic_indexer_block_getter_per_instantiation_5339,
           issue_5339_t_typed_string_int_class) {
    JitResult r = runAot(k_t_typed);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n3\n7\n"));
}

// FIX ROW. sum = sum_{k<200}(22+k + 11+k + 22+k + 11+k + 5+k) = 200*71 + 5*19900.
BEZEL_TEST(generic_indexer_block_getter_per_instantiation_5339,
           via_generic_method_derived_and_loop) {
    JitResult r = runAot(k_via_method_derived_loop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("113700\n"));
}

// CONTROL ROW (passes in every leg): non-generic owner, one instantiation.
BEZEL_TEST(generic_indexer_block_getter_per_instantiation_5339,
           control_nongeneric_and_single_instantiation) {
    JitResult r = runAot(k_control);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n12\n32\n"));
}
