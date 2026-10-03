// fix #5316 (§14.1 "Generic declarations" / §27.6 "Method references" / §12
// "Inheritance", v0.313): a property reference through a DERIVED receiver of a
// generic base (`DS : Holder<string>` forming `this.v`, or `a.v` for `a: DS`)
// installed no §14.1 substitution in makePropertyThunk, so signature and body
// lowered against the erased `T -> i32` placeholder: `""` instead of `"z"`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Reviewer probe pd.bzl: non-generic derived classes forming `this.v`.
constexpr const char* k_derived_this =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "end Holder;\n"
    "class DI : Holder<int>\n"
    "  public constructor() base(3); end constructor;\n"
    "  public method Ref(): () -> int\n"
    "    return this.v;\n"
    "  end Ref;\n"
    "end DI;\n"
    "class DS : Holder<string>\n"
    "  public constructor() base(\"z\"); end constructor;\n"
    "  public method Ref(): () -> string\n"
    "    return this.v;\n"
    "  end Ref;\n"
    "end DS;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var a := new DI();\n"
    "    var b := new DS();\n"
    "    Console.WriteLine(a.Ref()().ToString());\n"
    "    Console.WriteLine(b.Ref()());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The reference formed from OUTSIDE, through a derived receiver expression
// (`a.v` with `a: DS`), plus an int sibling whose offset moves.
constexpr const char* k_derived_outside =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(t: T, y: int) v := t; w := y; end constructor;\n"
    "end Holder;\n"
    "class DI : Holder<int>\n"
    "  public constructor() base(3, 11); end constructor;\n"
    "end DI;\n"
    "class DS : Holder<string>\n"
    "  public constructor() base(\"z\", 22); end constructor;\n"
    "end DS;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var a := new DI();\n"
    "    var b := new DS();\n"
    "    var fa: () -> int := a.v;\n"
    "    var fb: () -> string := b.v;\n"
    "    var wa: () -> int := a.w;\n"
    "    var wb: () -> int := b.w;\n"
    "    Console.WriteLine(fa());\n"
    "    Console.WriteLine(fb());\n"
    "    Console.WriteLine(wa());\n"
    "    Console.WriteLine(wb());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL (passes pre-fix: a specialized body carries its own substitution).
// A GENERIC derived class (`Derived<T> : Holder<T>`) at two instantiations,
// `this.v` formed inside its methods.
constexpr const char* k_derived_generic =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(t: T, y: int) v := t; w := y; end constructor;\n"
    "end Holder;\n"
    "class Derived<T> : Holder<T>\n"
    "  public constructor(t: T, y: int) base(t, y); end constructor;\n"
    "  public method RefV(): () -> T\n"
    "    return this.v;\n"
    "  end RefV;\n"
    "  public method RefW(): () -> int\n"
    "    return this.w;\n"
    "  end RefW;\n"
    "end Derived;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var di := new Derived<int>(3, 11);\n"
    "    var ds := new Derived<string>(\"z\", 22);\n"
    "    Console.WriteLine(di.RefV()());\n"
    "    Console.WriteLine(ds.RefV()());\n"
    "    Console.WriteLine(di.RefW()());\n"
    "    Console.WriteLine(ds.RefW()());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Record-typed and nested generic argument through a derived class.
constexpr const char* k_derived_nested =
    "module M;\n"
    "record Pair(A: int, B: int);\n"
    "class Box<T>\n"
    "  public property item: T;\n"
    "  public constructor(t: T) item := t; end constructor;\n"
    "end Box;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "end Holder;\n"
    "class DP : Holder<Pair>\n"
    "  public constructor() base(new Pair(7, 8)); end constructor;\n"
    "  public method Ref(): () -> Pair\n"
    "    return this.v;\n"
    "  end Ref;\n"
    "end DP;\n"
    "class DB : Holder<Box<string>>\n"
    "  public constructor() base(new Box<string>(\"q\")); end constructor;\n"
    "  public method Ref(): () -> Box<string>\n"
    "    return this.v;\n"
    "  end Ref;\n"
    "end DB;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var p := new DP();\n"
    "    var b := new DB();\n"
    "    var rp := p.Ref()();\n"
    "    Console.WriteLine(rp.A + rp.B);\n"
    "    Console.WriteLine(b.Ref()().item);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// Issue repro. FAIL-WITHOUT: `3` then an empty line.
BEZEL_TEST(generic_property_derived_receiver_5316,
           issue_5316_this_v_in_derived_of_generic_base) {
    JitResult r = runAot(k_derived_this);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n"));
    JitResult c = runAotChecked(k_derived_this);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("3\nz\n"));
}

BEZEL_TEST(generic_property_derived_receiver_5316,
           reference_through_derived_receiver_expression) {
    JitResult r = runAot(k_derived_outside);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n11\n22\n"));
}

BEZEL_TEST(generic_property_derived_receiver_5316,
           control_generic_derived_specialized_methods) {
    JitResult r = runAot(k_derived_generic);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n11\n22\n"));
}

BEZEL_TEST(generic_property_derived_receiver_5316,
           derived_of_record_and_nested_generic_argument) {
    JitResult r = runAot(k_derived_nested);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("15\nq\n"));
}
