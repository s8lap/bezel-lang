// fix #5315 (§14.1 "Generic declarations" / §23 "Properties" / §27.6 "Method
// references", v0.313): `ensurePropertyBlockGetter` memoized a block-bodied
// getter per (PropertyDecl, accessor tag 0), so the first generic instantiation
// to need `get => do ... end` froze its signature and field-GEP layout for
// every later instantiation. Through a §27.6 property reference (`var c: () ->
// int := hs.bw`) the memo is now keyed on the owner's instantiation, the same
// `propertyThunkSubstKey` #5306 uses for the thunk. (The INLINE read face,
// `hs.bw` as a plain read, installs its substitution at call sites in
// LlvmBackend_Exprs.cpp and is NOT covered here.)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Issue repro 2 (q/m_block_thunk_only.bzl): only the int-typed block getter,
// referenced at two instantiations.
constexpr const char* k_block_int_only =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property bw: int get => do return _w; end; end;\n"
    "  public constructor(x: T, y: int)\n"
    "    _v := x;\n"
    "    _w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    var c: () -> int := hi.bw;\n"
    "    var d: () -> int := hs.bw;\n"
    "    Console.WriteLine(c());\n"
    "    Console.WriteLine(d());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The T-typed block getter too (signature differs per instantiation), three
// instantiations including a class-typed T.
constexpr const char* k_block_t_int_str =
    "module M;\n"
    "class Pt\n"
    "  public property X: int;\n"
    "  public constructor(x: int) X := x; end constructor;\n"
    "end Pt;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public _w: int;\n"
    "  public property bv: T get => do return _v; end; end;\n"
    "  public property bw: int get => do return _w; end; end;\n"
    "  public constructor(x: T, y: int)\n"
    "    _v := x;\n"
    "    _w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var hp := new Holder<Pt>(new Pt(7), 33);\n"
    "    var a: () -> string := hs.bv;\n"
    "    var b: () -> int := hi.bv;\n"
    "    var c: () -> Pt := hp.bv;\n"
    "    var d: () -> int := hs.bw;\n"
    "    var e: () -> int := hi.bw;\n"
    "    var f: () -> int := hp.bw;\n"
    "    Console.WriteLine(a());\n"
    "    Console.WriteLine(b());\n"
    "    Console.WriteLine(c().X);\n"
    "    Console.WriteLine(d());\n"
    "    Console.WriteLine(e());\n"
    "    Console.WriteLine(f());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Two references at the SAME instantiation still compare equal
// (§27.7.1), and a non-generic owner is unaffected.
constexpr const char* k_block_equality =
    "module M;\n"
    "class Holder<T>\n"
    "  public _v: T;\n"
    "  public property bv: T get => do return _v; end; end;\n"
    "  public constructor(x: T) _v := x; end constructor;\n"
    "end Holder;\n"
    "class Plain\n"
    "  public property p: int get => do return 5; end; end;\n"
    "end Plain;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var pl := new Plain();\n"
    "    var a1: () -> int := hi.bv;\n"
    "    var a2: () -> int := hi.bv;\n"
    "    var b1: () -> string := hs.bv;\n"
    "    var b2: () -> string := hs.bv;\n"
    "    var p1: () -> int := pl.p;\n"
    "    var p2: () -> int := pl.p;\n"
    "    Console.WriteLine(a1 = a2);\n"
    "    Console.WriteLine(b1 = b2);\n"
    "    Console.WriteLine(p1 = p2);\n"
    "    Console.WriteLine(p1());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// Issue repro 2. FAIL-WITHOUT: `11` then a garbage pointer-sized value.
BEZEL_TEST(generic_property_block_getter_per_instantiation_5315,
           issue_5315_block_getter_int_sibling_through_reference) {
    JitResult r = runAot(k_block_int_only);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n22\n"));
    JitResult c = runAotChecked(k_block_int_only);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("11\n22\n"));
}

BEZEL_TEST(generic_property_block_getter_per_instantiation_5315,
           block_getter_three_instantiations_string_int_class) {
    JitResult r = runAot(k_block_t_int_str);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n3\n7\n22\n11\n33\n"));
}

// Two references at one instantiation stay equal (§27.7.1). FAIL-WITHOUT: does
// not compile (the shared getter mismatches the second instantiation).
BEZEL_TEST(generic_property_block_getter_per_instantiation_5315,
           same_instantiation_block_references_compare_equal) {
    JitResult r = runAot(k_block_equality);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\nTrue\n5\n"));
}
