// Spec-derived regression test for issue #6741.
//
// §22.10 (Primary constructors / the inheritance clause's base call), v0.340.
// Quoted: a primary-constructor class "desugars to a primary constructor that
// chains to `base(...)` before executing field initializers". So in
// `class D(m): C(m)` over `class C: A` with an explicit
// `constructor(x) base(x + "!")` over `class A(m)`, EVERY ancestor's positional
// field is stored before any field initializer runs (§22.8 step 0-2).
//
// THE DEFECT. `prePopulateBasePositionalFields(D, ...)` stops at its top gate
// because `C` declares no primary parameter list of its own, and
// `emitImplicitBase` then CALLS `C`'s compiled constructor, whose
// `base(x + "!")` into the ctor-less primary-ctor `A` deliberately stores
// nothing (it assumes pre-population at the `new` site). MEASURED on e3b1c403:
// a user root SEGVs reading `A`'s positional-backed field (exit 139); an
// `Exception` root has an empty `Message`. `new C(..)` directly was right.
//
// ORACLE. `runAot` pins exact stdout; the string-owning rows run 200x under
// `runAotChecked` (generated-code ASan + LSan), because the fix stores owned
// strings into the base slots (use of uninitialised memory / leak / double
// release would be loud there).

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "support/codegen_support.hpp"      // JitResult
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectStdout(const JitResult& r, const std::string& want, bool checked) {
    if (checked) BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

BEZEL_TEST(codegen_chain_through_explicit_ctor_primary_base_6741,
           issue_repro_user_base_positional_field_stored_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    const std::string src =
        "module P4;\n"
        "public class A(m: string)\n"
        "  public Msg: string := m;\n"
        "end A;\n"
        "public class C: A\n"
        "  public constructor(x: string)\n"
        "    base(x + \"!\");\n"
        "  end constructor;\n"
        "end C;\n"
        "public class D(m: string): C(m)\n"
        "end D;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var c: C := new C(\"c\");\n"
        "    Console.WriteLine($\"C [{c.Msg}]\");\n"
        "    using var d: D := new D(\"d\");\n"
        "    Console.WriteLine($\"D [{d.Msg}]\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    expectStdout(runAot(src), "C [c!]\nD [d!]\n", false);
}

BEZEL_TEST(codegen_chain_through_explicit_ctor_primary_base_6741,
           issue_repro_exception_base_message_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    const std::string src =
        "module P3;\n"
        "public class A(m: string): Exception(m)\n"
        "end A;\n"
        "public class C: A\n"
        "  public constructor(x: string)\n"
        "    base(x + \"!\");\n"
        "  end constructor;\n"
        "end C;\n"
        "public class D(m: string): C(m)\n"
        "end D;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      throw new C(\"c\");\n"
        "    catch var e: A =>\n"
        "      Console.WriteLine($\"C [{e.Message}]\");\n"
        "    end;\n"
        "    try\n"
        "      throw new D(\"d\");\n"
        "    catch var e: A =>\n"
        "      Console.WriteLine($\"D [{e.Message}]\");\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    expectStdout(runAot(src), "C [c!]\nD [d!]\n", false);
}

BEZEL_TEST(codegen_chain_through_explicit_ctor_primary_base_6741,
           depth4_user_base_field_initializers_each_level_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    const std::string src =
        "module M1;\n"
        "public class A(m: string)\n"
        "  public FA: string := m + \"a\";\n"
        "  public Msg: string := m;\n"
        "end A;\n"
        "public class C: A\n"
        "  public FC: string := \"c\";\n"
        "  public constructor(x: string)\n"
        "    base(x + \"!\");\n"
        "  end constructor;\n"
        "end C;\n"
        "public class D(m: string): C(m)\n"
        "  public FD: string := m + \"d\";\n"
        "end D;\n"
        "public class E(m: string): D(m)\n"
        "  public FE: string := m + \"e\";\n"
        "end E;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var e: E := new E(\"q\");\n"
        "    Console.WriteLine($\"{e.Msg} {e.FA} {e.FC} {e.FD} {e.FE}\");\n"
        "    using var d: D := new D(\"r\");\n"
        "    Console.WriteLine($\"{d.Msg} {d.FA} {d.FC} {d.FD}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    expectStdout(runAot(src), "q! q!a c qd qe\nr! r!a c rd\n", false);
}

BEZEL_TEST(codegen_chain_through_explicit_ctor_primary_base_6741,
           two_explicit_levels_between_primary_classes_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    const std::string src =
        "module M2;\n"
        "public class A(m: string)\n"
        "  public Msg: string := m;\n"
        "end A;\n"
        "public class C: A\n"
        "  public constructor(x: string)\n"
        "    base(x + \"!\");\n"
        "  end constructor;\n"
        "end C;\n"
        "public class D: C\n"
        "  public constructor(y: string)\n"
        "    base(y + \"?\");\n"
        "  end constructor;\n"
        "end D;\n"
        "public class E(m: string): D(m)\n"
        "end E;\n"
        "public class F(m: string): E(m)\n"
        "  public Z: string := m + \"z\";\n"
        "end F;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var e: E := new E(\"e\");\n"
        "    Console.WriteLine($\"{e.Msg}\");\n"
        "    using var f: F := new F(\"f\");\n"
        "    Console.WriteLine($\"{f.Msg} {f.Z}\");\n"
        "    using var d: D := new D(\"d\");\n"
        "    Console.WriteLine($\"{d.Msg}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    expectStdout(runAot(src), "e?!\nf?! fz\nd?!\n", false);
}

BEZEL_TEST(codegen_chain_through_explicit_ctor_primary_base_6741,
           this_chaining_explicit_ctor_in_the_middle_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    const std::string src =
        "module M7;\n"
        "public class A(m: string)\n"
        "  public Msg: string := m;\n"
        "end A;\n"
        "public class C: A\n"
        "  public Tag: int := 7;\n"
        "  public constructor(x: string, k: int)\n"
        "    base(x + \"!\");\n"
        "  end constructor;\n"
        "  public constructor(x: string)\n"
        "    this(x + \"?\", 1);\n"
        "  end constructor;\n"
        "end C;\n"
        "public class D(m: string): C(m)\n"
        "end D;\n"
        "public class D2(m: string): C(m, 3)\n"
        "end D2;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var d: D := new D(\"d\");\n"
        "    Console.WriteLine($\"{d.Msg} {d.Tag}\");\n"
        "    using var d2: D2 := new D2(\"e\");\n"
        "    Console.WriteLine($\"{d2.Msg} {d2.Tag}\");\n"
        "    using var c: C := new C(\"c\");\n"
        "    Console.WriteLine($\"{c.Msg} {c.Tag}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    expectStdout(runAot(src), "d?! 7\ne! 7\nc?! 7\n", false);
}

BEZEL_TEST(codegen_chain_through_explicit_ctor_primary_base_6741,
           argument_exception_param_name_through_chain_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    const std::string src =
        "module M5;\n"
        "public class A(m: string, p: string): ArgumentException(m, p)\n"
        "end A;\n"
        "public class C: A\n"
        "  public constructor(x: string)\n"
        "    base(x + \"!\", \"pn\");\n"
        "  end constructor;\n"
        "end C;\n"
        "public class D(m: string): C(m)\n"
        "end D;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    try\n"
        "      throw new D(\"d\");\n"
        "    catch var e: ArgumentException =>\n"
        "      Console.WriteLine($\"{e.Message} {e.ParamName}\");\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    expectStdout(runAot(src), "d! pn\n", false);
}

BEZEL_TEST(codegen_chain_through_explicit_ctor_primary_base_6741,
           temp_argument_in_clause_owned_once_loop_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    const std::string src =
        "module M3;\n"
        "public class A(m: string, n: int)\n"
        "  public Msg: string := m;\n"
        "  public N: int := n;\n"
        "end A;\n"
        "public class C: A\n"
        "  public constructor(x: string, k: int)\n"
        "    base(x + \"!\", k + 1);\n"
        "  end constructor;\n"
        "end C;\n"
        "public class D(m: string): C($\"{m}-t\", 5)\n"
        "end D;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var i: int := 0;\n"
        "    var t: int := 0;\n"
        "    while i < 200 do\n"
        "      i := i + 1;\n"
        "      using var d: D := new D(\"d\" + i.ToString());\n"
        "      t := t + d.Msg.Length + d.N;\n"
        "    end while;\n"
        "    Console.WriteLine($\"{t}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    expectStdout(runAotChecked(src), "2492\n", true);
}

BEZEL_TEST(codegen_chain_through_explicit_ctor_primary_base_6741,
           exception_root_depth4_message_and_field_loop_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    const std::string src =
        "module M4;\n"
        "public class A(m: string): Exception(m)\n"
        "  public FA: int := m.Length;\n"
        "end A;\n"
        "public class C: A\n"
        "  public constructor(x: string)\n"
        "    base(x + \"!\");\n"
        "  end constructor;\n"
        "end C;\n"
        "public class D(m: string): C(m)\n"
        "end D;\n"
        "public class E(m: string): D(m)\n"
        "  public FE: int := m.Length + 100;\n"
        "end E;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var i: int := 0;\n"
        "    var t: int := 0;\n"
        "    while i < 200 do\n"
        "      i := i + 1;\n"
        "      try\n"
        "        throw new E(\"m\" + i.ToString());\n"
        "      catch var e: A =>\n"
        "        t := t + e.Message.Length + e.FA;\n"
        "      end;\n"
        "    end while;\n"
        "    Console.WriteLine($\"{t}\");\n"
        "    try\n"
        "      throw new E(\"deep\");\n"
        "    catch var e: Exception =>\n"
        "      Console.WriteLine(e.Message);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    expectStdout(runAotChecked(src), "1784\ndeep!\n", true);
}
