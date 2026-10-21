// Spec-derived regression test for issue #6704.
//
// §22.10 (Primary constructors / the inheritance clause's base call), v0.339.
// Quoted: the primary-constructor parameters are in scope in "the inheritance
// clause's base call", and the class "desugars to a primary constructor that
// chains to `base(...)` before executing field initializers". §31.4 declares
// `Message` of every built-in exception type.
//
// THE DEFECT. `public class MyError(msg: string): Exception(msg)` threw and
// caught with an EMPTY `e.Message`. A built-in exception has no TypeSymbol, so
// `Emitter::emitImplicitBase` (the lowering of the clause's base call) returned
// at `baseClassOf(cls) == nullptr` and never lowered the clause arguments; the
// explicit `constructor(msg) base(msg)` spelling reached the built-in arm of
// `emitChainCall` and worked. MEASURED on 4e6de508: every cell below with a
// built-in exception base printed an empty Message (literal, parameter,
// expression, multi-argument, 2-level chain alike); the user-base controls
// were already right.
//
// The oracle is the checked AOT run (generated-code ASan + LSan). Each Main returns
// a distinct non-zero code on a mismatch.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::JitResult;
using bezel::test::runAotChecked;

int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// The issue's literal repro, thrown and caught.
BEZEL_TEST(codegen_primary_ctor_builtin_exception_base_6704,
           issue_repro_thrown_and_caught_spec_22_10) {
    // per §22.10 "the inheritance clause's base call"
    BEZEL_EXPECT_EQ(run(
        "module M6704A; "
        "public class MyError(msg: string): Exception(msg) "
        "end MyError; "
        "public class Program "
        "  public static method Main(): int "
        "    var ok: bool := false; "
        "    try "
        "      throw new MyError(\"too big\"); "
        "    catch var e: MyError => ok := e.Message = \"too big\"; "
        "    end; "
        "    if not ok then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 0);
}

// Built-in subclasses; parameter, literal, expressions, multi-argument.
BEZEL_TEST(codegen_primary_ctor_builtin_exception_base_6704,
           builtin_subclasses_and_argument_shapes_spec_22_10_31_4) {
    // per §22.10 / §31.4
    BEZEL_EXPECT_EQ(run(
        "module M6704B; "
        "public class EA(msg: string): ArgumentException(msg) end EA; "
        "public class EI(msg: string): InvalidOperationException(msg) end EI; "
        "public class EO(msg: string): IOException(msg) end EO; "
        "public class EX(msg: string): Exception($\"x {msg}\") end EX; "
        "public class EC(msg: string): Exception(msg + \"!\") end EC; "
        "public class EL(n: int): Exception(\"lit\") end EL; "
        "public class EP(msg: string, p: string): ArgumentException(msg, p) "
        "end EP; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a: EA := new EA(\"a\"); "
        "    if a.Message <> \"a\" then return 1; end if; "
        "    using var i: EI := new EI(\"i\"); "
        "    if i.Message <> \"i\" then return 2; end if; "
        "    using var o: EO := new EO(\"o\"); "
        "    if o.Message <> \"o\" then return 3; end if; "
        "    using var x: EX := new EX(\"q\"); "
        "    if x.Message <> \"x q\" then return 4; end if; "
        "    using var c: EC := new EC(\"c\"); "
        "    if c.Message <> \"c!\" then return 5; end if; "
        "    using var l: EL := new EL(3); "
        "    if l.Message <> \"lit\" then return 6; end if; "
        "    using var p: EP := new EP(\"pm\", \"pn\"); "
        "    if p.Message <> \"pm\" then return 7; end if; "
        "    var pb: ArgumentException := p; "
        "    var pn: string? := pb.ParamName; "
        "    if pn <> null then "
        "      if pn <> \"pn\" then return 8; end if; "
        "    else "
        "      return 9; "
        "    end if; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 0);
}

// Field initializers that read the parameter run after the base call; a
// 2-level chain; Message read through the caught base type.
BEZEL_TEST(codegen_primary_ctor_builtin_exception_base_6704,
           fields_and_chain_depth_spec_22_10) {
    // per §22.10 "chains to base(...) before executing field initializers"
    BEZEL_EXPECT_EQ(run(
        "module M6704C; "
        "public class F(msg: string): Exception(msg) "
        "  public Len: int := msg.Length; "
        "end F; "
        "public class A(m: string): Exception(m) end A; "
        "public class B(m: string): A(m) "
        "  public Tag: int := m.Length + 1; "
        "end B; "
        "public class Program "
        "  public static method Main(): int "
        "    var ok1: bool := false; "
        "    try "
        "      throw new F(\"abc\"); "
        "    catch var e: Exception => ok1 := e.Message = \"abc\"; "
        "    end; "
        "    if not ok1 then return 1; end if; "
        "    using var f: F := new F(\"abcd\"); "
        "    if f.Len <> 4 then return 2; end if; "
        "    var ok3: bool := false; "
        "    try "
        "      throw new B(\"deep\"); "
        "    catch var e: Exception => ok3 := e.Message = \"deep\"; "
        "    end; "
        "    if not ok3 then return 3; end if; "
        "    using var b: B := new B(\"xy\"); "
        "    if b.Message <> \"xy\" then return 4; end if; "
        "    if b.Tag <> 3 then return 5; end if; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 0);
}

// Controls: user bases were already right and must stay so.
BEZEL_TEST(codegen_primary_ctor_builtin_exception_base_6704,
           user_base_controls_spec_22_10) {
    // per §22.10
    BEZEL_EXPECT_EQ(run(
        "module M6704D; "
        "public class UA(m: string) "
        "  public Msg: string := m; "
        "end UA; "
        "public class UB(m: string): UA(m) end UB; "
        "public class XA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end XA; "
        "public class XB(m: string): XA(m) end XB; "
        "public class Program "
        "  public static method Main(): int "
        "    using var u: UB := new UB(\"ub\"); "
        "    if u.Msg <> \"ub\" then return 1; end if; "
        "    using var x: XB := new XB(\"xb\"); "
        "    if x.Message <> \"xb\" then return 2; end if; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 0);
}

// Ownership: a runtime-built (non-literal) message forwarded through the
// clause is retained by the slot and released exactly once. 200 iterations so
// a one-string-per-instance leak or a use-after-free is loud under ASan/LSan.
BEZEL_TEST(codegen_primary_ctor_builtin_exception_base_6704,
           runtime_message_owned_exactly_once_loop_spec_22_10_28_9) {
    // per §22.10 / §28.9 / §28.4 step 3
    BEZEL_EXPECT_EQ(run(
        "module M6704E; "
        "public class E(msg: string): Exception(msg) end E; "
        "public class Program "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      i := i + 1; "
        "      try "
        "        throw new E(\"m\" + i.ToString()); "
        "      catch var e: E => t := t + e.Message.Length; "
        "      end; "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + churn.Length; "
        "    end while; "
        // 9x(2+2) + 90x(3+3) + 101x(4+4) = 36 + 540 + 808 = 1384
        "    if t <> 1384 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 0);
}
