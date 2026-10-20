// Regression tests for #6224: a class listing ONE generic interface at TWO
// instantiations, called through the interface from inside a GENERIC body
// (receiver typed `IL<X>`, X a type parameter of the enclosing method or
// class), ran the first-listed instantiation's overload for every X.
//
// Spec §11.1 (v0.339): "A generic interface may be listed at two or more
// instantiations ... §11.2's matching rule is applied once per obligation,
// each under its own instantiation's substitution". §22.6: `i.Put(v)` with
// `v: X` selects `Put(x: T)` at `T = X`, so `Use<string>` reaches
// `Put(x: string)`.
//
// Cause: the itable-dispatch site (emitMethodCall) selects the per-
// instantiation itab from the receiver's static type read through
// genericSubstSemaTy_. At a call lowered inside a monomorphized generic body
// that environment binds the INTERFACE's own parameters (`T`) to the
// receiver's arguments, not the caller's `X`, so `IL<X>` stayed open and the
// shared itab (the first instantiation) was used.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
const char* kTwo =
    "interface IL<T> method Put(x: T): int; method Put(x: bool): int; end IL;\n"
    "class Two: IL<int>, IL<string>\n"
    "  public method Put(x: bool): int => 7;\n"
    "  public method Put(x: string): int => 8;\n"
    "  public method Put(x: int): int => 9;\n"
    "end Two;\n";
}  // namespace

// The issue's literal repro: expected 9 then 8 (was 7, 7; then 9, 9).
BEZEL_TEST(generic_iface_two_inst_from_generic_body_6224,
           issue_repro_generic_method_spec_11_1_22_6) {
    auto r = runAotChecked(
        std::string("module M;\n") + kTwo +
        "static class S\n"
        "  public static method Use<X>(i: IL<X>, v: X): int => i.Put(v);\n"
        "end S;\n"
        "class P public static method Main(): int\n"
        "  using var t := new Two();\n"
        "  Console.WriteLine(S.Use<int>(t, 4));\n"
        "  Console.WriteLine(S.Use<string>(t, \"s\"));\n"
        "  return 0; end Main; end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n8\n"));
}

// Same through a generic CLASS's method (X is the class's parameter), with
// the string instantiation first so a first-listed pick cannot pass by luck.
BEZEL_TEST(generic_iface_two_inst_from_generic_body_6224,
           generic_class_method_spec_11_1_22_6) {
    auto r = runAotChecked(
        std::string("module M;\n") + kTwo +
        "class H<X>\n"
        "  public method Use(i: IL<X>, v: X): int => i.Put(v);\n"
        "end H;\n"
        "class P public static method Main(): int\n"
        "  using var t := new Two();\n"
        "  using var hs := new H<string>(); using var hi := new H<int>();\n"
        "  Console.WriteLine(hs.Use(t, \"s\"));\n"
        "  Console.WriteLine(hi.Use(t, 4));\n"
        "  return 0; end Main; end P;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\n9\n"));
}
