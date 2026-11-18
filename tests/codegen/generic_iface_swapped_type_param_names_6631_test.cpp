// Regression test for #6631: a generic method whose type-parameter NAMES are
// SWAPPED relative to the interface's dispatches called the WRONG
// instantiation — the receiver `i: IM<U,T>` (interface `IM<T,U>`) resolved to
// the instantiation whose arguments are the OTHER way round, so the dispatch
// selected the other instantiation's itab and ran its witness with the
// arguments at the wrong types.
//
// Spec §11.1 (v0.359): "A generic interface may be listed at two or more
// instantiations ... the class still declares one member set, and §11.2's
// matching rule is applied once per obligation, each under its own
// instantiation's substitution". §11.2 *Generic members* (v0.359): "The
// interface's substitution maps each of the interface's type parameters to the
// implementing type's argument" — by POSITION, not by name. §14.1 *Instantiation
// and its site* (v0.359): a site whose type arguments are type parameters in
// scope "forms, at each instantiation of the declaration that declares the
// parameter ... the instantiation its arguments substitute to" — the CALLER's
// `U` and `T`, positionally.
//
// Cause: the itable-dispatch site (emitMethodCall) read the receiver's static
// type through the active substitution, which at that point binds the
// INTERFACE's own parameter names (`T`, `U`) — so the receiver's arguments,
// spelled with the CALLER's parameter names, were captured by name and the
// instantiation came out swapped. The instantiation is now resolved
// POSITIONALLY, in the scope the receiver's type was written in.
//
// Expected: `Use<int,string>` reaches `Put(a: int, b: string)` (prints 1) and
// `Use<string,int>` reaches `Put(a: string, b: int)` (prints 2).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// The issue's literal repro: the interface declares `IM<T,U>`, the generic
// method declares `Use<U,T>` — the names swapped.
const char* kSwapped =
    "module M;\n"
    "public interface IM<T, U>\n"
    "  method Put(a: T, b: U): int;\n"
    "end IM;\n"
    "public class Impl: IM<int, string>, IM<string, int>\n"
    "  public constructor() end constructor;\n"
    "  public method Put(a: int, b: string): int => 1;\n"
    "  public method Put(a: string, b: int): int => 2;\n"
    "end Impl;\n";
}  // namespace

// The issue's literal repro: expected 1 then 2 (was 2 then 1).
BEZEL_TEST(generic_iface_swapped_type_param_names_6631,
           issue_repro_spec_11_1_11_2_14_1) {
    auto r = runAotChecked(
        std::string(kSwapped) +
        "public class Program\n"
        "  public static method Use<U, T>(i: IM<U, T>, a: U, b: T): int =>"
        " i.Put(a, b);\n"
        "  public static method Main()\n"
        "    var t := new Impl();\n"
        "    Console.WriteLine(Program.Use<int, string>(t, 4, \"s\"));\n"
        "    Console.WriteLine(Program.Use<string, int>(t, \"s\", 4));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n"));
}

// The same capture with the enclosing scope a GENERIC CLASS's type parameters
// rather than a generic method's: `Holder<T,U>.Use(i: IM<U,T>)` at
// `Holder<int,string>` is `IM<string,int>` — the witness `Put(a: string,
// b: int)` (prints 2). On the base the receiver resolved to `IM<int,string>`
// and printed 1.
BEZEL_TEST(generic_iface_swapped_type_param_names_6631,
           generic_class_scope_spec_11_1_11_2_14_1) {
    auto r = runAotChecked(
        std::string(kSwapped) +
        "public class Holder<T, U>\n"
        "  public method Use(i: IM<U, T>, a: U, b: T): int => i.Put(a, b);\n"
        "end Holder;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var t := new Impl();\n"
        "    using var h := new Holder<int, string>();\n"
        "    Console.WriteLine(h.Use(t, \"s\", 4));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// The capture with THREE type parameters, the receiver's arguments a
// three-cycle (`IM<V,T,U>`): `Use<int,string,bool>` instantiates the receiver
// at `IM<int,string,bool>` — the witness `Put(a: int, b: string, c: bool)`
// (prints 1) — and `Use<string,int,bool>` at `IM<string,int,bool>` (prints 2).
// On the base both calls reached the first instantiation (1 then 1).
BEZEL_TEST(generic_iface_swapped_type_param_names_6631,
           three_type_parameters_spec_11_1_11_2_14_1) {
    auto r = runAotChecked(
        std::string("module M;\n"
                    "public interface IM<T, U, V>\n"
                    "  method Put(a: T, b: U, c: V): int;\n"
                    "end IM;\n"
                    "public class Impl: IM<int, string, bool>,"
                    " IM<string, int, bool>\n"
                    "  public constructor() end constructor;\n"
                    "  public method Put(a: int, b: string, c: bool): int => 1;\n"
                    "  public method Put(a: string, b: int, c: bool): int => 2;\n"
                    "end Impl;\n"
                    "public class Program\n"
                    "  public static method Use<V, T, U>(i: IM<V, T, U>,"
                    " a: T, b: U, c: V): int => i.Put(a, b, c);\n"
                    "  public static method Main()\n"
                    "    var t := new Impl();\n"
                    "    Console.WriteLine(Program.Use<int, string, bool>"
                    "(t, \"s\", true, 4));\n"
                    "    Console.WriteLine(Program.Use<string, int, bool>"
                    "(t, 4, true, \"s\"));\n"
                    "  end Main;\n"
                    "end Program;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n"));
}
