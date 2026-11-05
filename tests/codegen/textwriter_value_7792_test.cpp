// Regression for #7792 -- a `TextWriter` VALUE had no runtime representation.
//
// per stdlib §2.1 (v0.355): `Console` declares
//     public static readonly property Out: TextWriter;
//     public static readonly property Error: TextWriter;
// and per stdlib §7.4 (v0.355) `TextWriter`'s `Write` / `WriteLine` members
// are "[§2.1]'s rule, of which `Console.Out` and `Console.Error` are
// instances". So a `TextWriter` is an ordinary value: it may be passed as a
// parameter, held in a local that is later reassigned, stored in a field and
// returned, and `w.WriteLine(x)` writes to whichever stream `w` holds.
//
// Before the fix `Console.Out` lowered to null, so only a local bound once to
// `Console.Out` / `Console.Error` and never written again could write (#7713,
// which read the stream off the initializer). A parameter, a field, a
// reassigned local and a call result were rejected with #2457's "no dispatch
// arm" error (plus "lowerExpr returned null ... #1386" at the call site), and
// the inferred `var w := Console.Out;` compiled with the #2434 "degraded to a
// zero-initialized value" warning.
//
// Now `Console.Out` / `Console.Error` lower to the stream's descriptor (the
// i32 a built-in class type's slot already is), and a `TextWriter` receiver
// writes to the descriptor it holds at run time.
//
// The harness captures stdout only, so a row that writes through
// `Console.Error` asserts that the text did NOT reach stdout.
//
// NOT COVERED (narrow #7792): a user subclass of `TextWriter` (stdlib §1
// *Extensibility* opens it) cannot be held in a `TextWriter` location by this
// compiler. The conversion is now a located compile error instead of an 8-byte
// pointer stored into the 4-byte slot (a heap overrun) or an IR verifier
// abort; the last rows pin that.

#include <memory>
#include <string>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic message of an in-process front-end + codegen run. `runAot`
// drops the diagnostics of a SUCCESSFUL compile, so a Warning's absence can
// only be observed here (the pattern of
// tests/codegen/generic_template_local_degrade_7257_test.cpp).
std::vector<std::string> compileMessages(const std::string& src, bool& rejected) {
    std::vector<std::string> out;
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (unit) {
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        if (!diags.hasErrors()) {
            auto llvmCtx = std::make_unique<llvm::LLVMContext>();
            (void)bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
        }
    }
    rejected = !unit || diags.hasErrors();
    for (const auto& d : diags.diagnostics()) out.push_back(d.message);
    return out;
}

// A user subclass of `TextWriter` (stdlib §7.4's abstract members overridden).
const char* kSub =
    "class W: TextWriter "
    "  public constructor() end constructor; "
    "  public override method Write(value: string) Console.Write(\"[\" + value + \"]\"); end Write; "
    "  public override method Write(value: char) Console.Write(\"<c>\"); end Write; "
    "  public override method Flush() end Flush; "
    "end W; ";

const char* kSubNeedle = "a user subclass of `TextWriter`; converting it to the "
                         "built-in `TextWriter` type is not implemented";

}  // namespace

// The issue's repro (tw.bzl): `Console.Out` is not null, and a `TextWriter`
// PARAMETER writes.
BEZEL_TEST(textwriter_value_7792, issue_repro_parameter_7_4) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  static method Emit(w: TextWriter, s: string) w.WriteLine(s); end Emit; "
        "  public static method Main(): int "
        "    var w: TextWriter? := Console.Out; "
        "    Console.WriteLine(w = null); "
        "    Emit(Console.Out, \"param\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nparam\n"));
}

// A parameter: every declared member, and the stream follows the argument
// (`Console.Error`'s writes do not reach stdout). The class argument is a
// named local: a temporary `new Cls()` at an `Object` parameter leaks on
// every spelling, `Console.WriteLine` included (#6975, not this issue).
BEZEL_TEST(textwriter_value_7792, parameter_dispatches_on_the_argument_2_1_7_4) {
    auto r = runAotChecked(
        "module M; "
        "class Cls public constructor() end constructor; "
        "  public override method ToString(): string => \"cls\"; end Cls; "
        "class C "
        "  static method Emit(w: TextWriter, s: string) "
        "    w.WriteLine(s); w.Write(\"x\"); w.Write('c'); w.Write(7); "
        "    w.Write(true); w.WriteLine(); var o: Object := 5; w.WriteLine(o); "
        "    var k := new Cls(); w.WriteLine(k); w.WriteLine($\"i{1 + 2}\"); w.Flush(); "
        "  end Emit; "
        "  public static method Main(): int "
        "    Emit(Console.Out, \"out\"); "
        "    Emit(Console.Error, \"err\"); "
        "    Emit(Console.Out, \"out2\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("out\nxc7True\n5\ncls\ni3\n"
                                "out2\nxc7True\n5\ncls\ni3\n"));
}

// A local reassigned between `Console.Out` and `Console.Error`: each write
// goes to the stream the local holds at that point. Annotated and inferred.
BEZEL_TEST(textwriter_value_7792, reassigned_local_follows_the_value_2_1) {
    auto ann = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var w: TextWriter := Console.Out; w.WriteLine(\"a\"); "
        "  w := Console.Error; w.WriteLine(\"b\"); "
        "  w := Console.Out; w.WriteLine(\"c\"); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(ann.ran);
    BEZEL_EXPECT_EQ(ann.stdoutText, std::string("a\nc\n"));
    auto inf = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var w := Console.Error; w.WriteLine(\"e1\"); "
        "  w := Console.Out; w.WriteLine(\"o1\"); "
        "  for var i in 0..<4 do "
        "    if i mod 2 = 0 then w := Console.Error; else w := Console.Out; end; "
        "    w.WriteLine(i); "
        "  end for; "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(inf.ran);
    BEZEL_EXPECT_EQ(inf.stdoutText, std::string("o1\n1\n3\n"));
}

// A field: set in a constructor, read through `this` and through a receiver,
// and reassigned.
BEZEL_TEST(textwriter_value_7792, field_holds_the_stream_7_4) {
    auto r = runAotChecked(
        "module M; "
        "class H "
        "  public W: TextWriter; "
        "  public constructor(w: TextWriter) W := w; end constructor; "
        "  public method Say(s: string) W.WriteLine(s); end Say; "
        "end H; "
        "class C public static method Main(): int "
        "  var h := new H(Console.Out); h.Say(\"field\"); h.W.WriteLine(\"direct\"); "
        "  h.W := Console.Error; h.Say(\"hidden\"); h.W.Write(\"hidden2\"); "
        "  h.W := Console.Out; h.Say(\"back\"); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("field\ndirect\nback\n"));
}

// A return value: written through directly and through a bound local.
BEZEL_TEST(textwriter_value_7792, return_value_carries_the_stream_7_4) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  static method Pick(e: bool): TextWriter "
        "    if e then return Console.Error; end; return Console.Out; "
        "  end Pick; "
        "  public static method Main(): int "
        "    Pick(false).WriteLine(\"ret\"); Pick(true).WriteLine(\"hidden\"); "
        "    var r := Pick(false); r.WriteLine(\"retlocal\"); "
        "    var e: TextWriter := Pick(true); e.WriteLine(\"hidden2\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ret\nretlocal\n"));
}

// A `TextWriter?` formal, local and default: the value wraps, compares with
// `null`, and writes once narrowed (§7.4).
BEZEL_TEST(textwriter_value_7792, nullable_textwriter_parameter_and_local_7_4) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  static method Maybe(s: string, w: TextWriter? := null) "
        "    if w <> null then w.WriteLine(s); else Console.WriteLine(\"none\"); end; "
        "  end Maybe; "
        "  public static method Main(): int "
        "    var w: TextWriter? := Console.Out; Console.WriteLine(w = null); "
        "    w := null; Console.WriteLine(w = null); "
        "    Maybe(\"some\", Console.Out); Maybe(\"x\", null); Maybe(\"y\"); "
        "    Maybe(\"hidden\", Console.Error); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("False\nTrue\nsome\nnone\nnone\n"));
}

// The argument of a write through a `TextWriter` value is a call-bounded
// borrow (stdlib §2.1): 200 writes of a string local and a boxed Object
// through a parameter leak nothing (the runAotChecked leak net).
BEZEL_TEST(textwriter_value_7792, parameter_writes_loop_no_leak_28_9) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  static method Emit(w: TextWriter) "
        "    var o: Object := 7; var s := \"a\" + 5.ToString(); "
        "    for var i in 0..<200 do w.Write(o); w.Write(s); w.WriteLine(o); end for; "
        "  end Emit; "
        "  public static method Main(): int Emit(Console.Out); return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText.size(), std::size_t(200 * 5));
}

// The inferred local (a06b.bzl): prints, and compiles with NO `internal:`
// degrade diagnostic (the #2434 "degraded to a zero-initialized value" note
// was emitted before the fix). The diagnostics are read in-process, since a
// successful `runAot` compile drops them.
BEZEL_TEST(textwriter_value_7792, inferred_local_has_no_degrade_warning_21_1) {
    const std::string src =
        "module M; "
        "class P public static method Main() "
        "  Console.WriteLine(\"start\"); "
        "  var w := Console.Out; w.WriteLine(\"inferred-out\"); "
        "  Console.WriteLine(\"end\"); "
        "end Main; end P;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("start\ninferred-out\nend\n"));
    bool rejected = true;
    auto msgs = compileMessages(src, rejected);
    BEZEL_EXPECT_FALSE(rejected);
    int internal = 0;
    for (const auto& m : msgs)
        if (m.find("internal:") != std::string::npos ||
            m.find("#2434") != std::string::npos)
            ++internal;
    BEZEL_EXPECT_EQ(internal, 0);
}

// A TextWriter local inside a property getter, a constructor, a static
// constructor, a destructor and a lambda body (the e02_members.bzl comment on
// the issue).
BEZEL_TEST(textwriter_value_7792, member_bodies_write_7_4) {
    auto r = runAotChecked(
        "module M; "
        "class Q "
        " public constructor() var w: TextWriter := Console.Out; w.WriteLine(\"ctor\"); end constructor; "
        " public property Pr: int "
        "  get => do var w: TextWriter := Console.Out; w.WriteLine(\"getter\"); return 1; end; "
        " end Pr; "
        " destructor var w: TextWriter := Console.Out; w.WriteLine(\"dtor\"); end destructor; "
        "end Q; "
        "class P public static method Main() "
        "  using var q := new Q(); var a := q.Pr; "
        "  var t: TextWriter := Console.Out; "
        "  var f := lambda(): int => do t.WriteLine(\"lam\"); return 1; end; "
        "  var c := f(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ctor\ngetter\nlam\ndtor\n"));
}

// Control: an un-narrowed `TextWriter?` receiver is still a §7.4 compile
// error (the precondition of reading the narrowed carrier's payload).
BEZEL_TEST(textwriter_value_7792, un_narrowed_nullable_receiver_rejected_7_4) {
    auto r = runAot(
        "module M; class C "
        "  static method Bad(w: TextWriter?) w.WriteLine(\"x\"); end Bad; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "on a nullable value requires narrowing it to non-null first"));
}

// narrow #7792: a user subclass converted to `TextWriter` is a located
// compile error at a parameter, a local, a field store and a return.
BEZEL_TEST(textwriter_value_7792, user_subclass_to_textwriter_rejected_not_silent) {
    const std::string pre = std::string("module M; ") + kSub;
    const char* bodies[] = {
        "class C static method Emit(w: TextWriter) w.WriteLine(\"hi\"); end Emit; "
        "public static method Main(): int Emit(new W()); return 0; end Main; end C;",
        "class C public static method Main(): int var w := new W(); "
        "var t: TextWriter := w; t.WriteLine(\"hi\"); return 0; end Main; end C;",
        "class H public T: TextWriter; public constructor(w: W) T := w; end constructor; end H; "
        "class C public static method Main(): int var w := new W(); var h := new H(w); "
        "h.T.WriteLine(\"hi\"); return 0; end Main; end C;",
        "class C static method Mk(w: W): TextWriter return w; end Mk; "
        "public static method Main(): int var w := new W(); Mk(w).WriteLine(\"hi\"); "
        "return 0; end Main; end C;",
    };
    for (const char* b : bodies) {
        auto r = runAot(pre + b);
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kSubNeedle));
    }
}

// Control: declaring the subclass stays admitted (stdlib §1 *Extensibility*),
// and so does calling its own declared overrides on its own type.
BEZEL_TEST(textwriter_value_7792, user_subclass_declaration_still_admitted_1) {
    auto r = runAotChecked(std::string("module M; ") + kSub +
        "class C public static method Main(): int var w := new W(); "
        "w.Write(\"a\"); w.Write('z'); w.Flush(); Console.WriteLine(); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[a]<c>\n"));
}
