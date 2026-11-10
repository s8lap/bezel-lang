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
// *Extensibility* opens it) cannot be held in a `TextWriter` field, parameter,
// return, element or generic argument by this compiler. That conversion is a
// located compile error instead of an 8-byte pointer stored into the 4-byte
// slot (a heap overrun) or an IR verifier abort. A LOCAL declared `TextWriter`
// holds the object itself, so its object uses (`is` / `as` a user type,
// `Object`, `=`, printing) run, and every use as a built-in `TextWriter` is
// rejected; declared `TextWriter?`, only its `= null` / `<> null` reads are
// admitted. A type test or cast INTO `TextWriter` (`as`, a typed `case`
// binding), and a CAST of a `TextWriter` value, is a located compile error;
// the `is` / `is not` FORMS ANSWER instead (Issue #8256; spec §12.7, stdlib
// §7.4, §17.4 / §19.3 -- a type test produces a `bool`, and the runtime can
// now answer it; the sibling test
// tests/sema/builtin_io_class_type_test_8256_cluster11_test.cpp pins the other
// half of the same contract). A `TextWriter` never given a stream is a §7.9 /
// §17.11 compile error, as for a user class. The rows after the first block
// pin all of that.
//
// narrow #7792, third fix-forward: `TextWriter?` is a member of codegen's
// nullable-VALUE family and a bare `Console.Out` / `Console.Error` operand of
// `??` / an `if` expression / a `case` expression is typed `TextWriter`, so
// every spelling that PRODUCES a TextWriter from a nullable one (`??`, the
// `if` / `case` arms, `?.`, `default`, record / class fields, array and
// Dictionary elements, a generic field, a lambda / async / iterator result)
// lowers to the right stream. The last block of rows runs each spelling with
// fd 0 opened read-write on an empty file (`runWithFd0Trap`) and asserts the
// stdout AND stderr text and that fd 0 received nothing -- the oracle under
// which the review measured a null `TextWriter?` writing through descriptor 0.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
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

// narrow #7792 (stdlib §7.4, v0.355) -- the fd-0 trap. A `TextWriter` value
// is a standard-stream descriptor, so a TextWriter produced WITHOUT the
// representation's constructor (a null `TextWriter?` on the left of `??`,
// before this fix) holds descriptor 0 and `w.WriteLine(..)` writes to
// STANDARD INPUT. `runAot` opens fd 0 on /dev/null read-only, where such a
// write fails silently and the row sees only "nothing reached stdout" -- a
// vacuous oracle. This helper compiles through the real Driver (the AOT path)
// and runs the executable with fd 0 opened READ-WRITE on an empty scratch file,
// capturing stdout and stderr; a row asserts the stderr text (stdlib §2.1:
// `Console.Error` writes to standard error) AND that the fd-0 file stayed
// empty. The reviewer measured the defect exactly this way.
struct Fd0TrapOutcome {
    bool compiled = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
    long fd0Bytes = -1;  // bytes written through descriptor 0; must stay 0
};

static const std::filesystem::path& wtDirTw7792() {
    return wtScratchDir("wt-tw7792");
}

static std::string slurp(const std::string& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

Fd0TrapOutcome runWithFd0Trap(const std::string& source) {
    auto dir = wtDirTw7792();
    std::string stem = unique_stem("bezel_tw7792");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    std::string outf = (dir / (stem + ".stdout")).string();
    std::string errf = (dir / (stem + ".stderr")).string();
    std::string fd0f = (dir / (stem + ".fd0")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    Fd0TrapOutcome o;
    bezel::Driver driver;
    // narrow #7792, fourth fix-forward: a hand-rolled `driver.compileFile`
    // runner does not honour `BEZEL_AOT_FORCE_SANITIZE` by itself
    // (docs/ops/lane-contract.md item 8), so under the ASan sweep this runner
    // was blind to the heap-buffer-overflow the generic-construction rows
    // below pin. Mirror `runAot`: the sweep's one bit becomes
    // `opts.sanitizeAddress`, and the child runs with the same
    // `detect_leaks=0:abort_on_error=0` options `runAot` uses, so a fired
    // report is a non-zero exit (97) that `expectStreams` refuses.
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = bezel::test::forceSanitizeAddress();
    bezel::CompileResult r = driver.compileFile(src, exe, /*deps=*/{}, opts);
    o.compiled = r.success;
    if (r.success) {
        pid_t pid = ::fork();
        if (pid == 0) {
            if (opts.sanitizeAddress)
                ::setenv("ASAN_OPTIONS",
                         "detect_leaks=0:abort_on_error=0:exitcode=97", 1);
            int fd0 = ::open(fd0f.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
            int fd1 = ::open(outf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            int fd2 = ::open(errf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd0 < 0 || fd1 < 0 || fd2 < 0) ::_exit(126);
            ::dup2(fd0, 0);
            ::dup2(fd1, 1);
            ::dup2(fd2, 2);
            ::close(fd0);
            ::close(fd1);
            ::close(fd2);
            ::execl(exe.c_str(), exe.c_str(), (char*)nullptr);
            ::_exit(127);
        }
        if (pid > 0) {
            int status = 0;
            ::waitpid(pid, &status, 0);
            if (WIFEXITED(status)) o.exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) o.exitCode = 128 + WTERMSIG(status);
            o.stdoutText = slurp(outf);
            o.stderrText = slurp(errf);
            struct stat st {};
            if (::stat(fd0f.c_str(), &st) == 0) o.fd0Bytes = (long)st.st_size;
        }
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    std::remove(outf.c_str());
    std::remove(errf.c_str());
    std::remove(fd0f.c_str());
    return o;
}

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

// narrow #7792 fix-forward: a LOCAL declared `TextWriter` and initialized with
// a user subclass holds the object itself, so every use that reads it AS that
// object runs (the review's f01 f02 f03 f07 c06 c08, which the first narrow
// fix rejected): `as` / `is` a user type, `Console.WriteLine` (the `Object`
// overload, stdlib §2.1), `=` against the subclass value and against `null`,
// an `Object` parameter, a `TextWriter?` local, an interpolation hole.
BEZEL_TEST(textwriter_value_7792, user_subclass_local_object_uses_run_7_4) {
    const std::string pre = std::string("module M; ") + kSub;
    struct Row { const char* body; const char* out; };
    const Row rows[] = {
        // f01: downcast back and type test.
        {"class P public static method Main() var w := new W(); var t: TextWriter := w; "
         "var back := t as W; if back <> null then back.Write(\"dc\"); end if; "
         "Console.WriteLine(t is W); end Main; end P;",
         "[dc]True\n"},
        // f02: printed through the `Object` overload, and compared.
        {"class P public static method Main() var w := new W(); var t: TextWriter := w; "
         "Console.WriteLine(t); Console.WriteLine(t = w); end Main; end P;",
         "M.W\nTrue\n"},
        // f03: passed to an `Object` parameter.
        {"class P static method Q(o: Object) Console.WriteLine(\"q\"); end Q; "
         "public static method Main() var w := new W(); var t: TextWriter := w; Q(t); "
         "end Main; end P;",
         "q\n"},
        // f07: the round trip keeps the object.
        {"class P public static method Main() var w := new W(); var t: TextWriter := w; "
         "var b := t as W; if b <> null then b.Write(\"rt\"); else Console.WriteLine(\"lost\"); "
         "end if; Console.WriteLine(); end Main; end P;",
         "[rt]\n"},
        // c06: compared with `null`.
        {"class P public static method Main() var w := new W(); var t: TextWriter := w; "
         "Console.WriteLine(t = null); Console.WriteLine(\"ok\"); end Main; end P;",
         "False\nok\n"},
        // c08: a `TextWriter?` local.
        {"class P public static method Main() var w := new W(); var t: TextWriter? := w; "
         "Console.WriteLine(t = null); end Main; end P;",
         "False\n"},
        // An interpolation hole renders the object (§18.5).
        {"class P public static method Main() var w := new W(); var t: TextWriter := w; "
         "Console.WriteLine($\"<{t}>\"); end Main; end P;",
         "<M.W>\n"},
    };
    for (const Row& row : rows) {
        auto r = runAotChecked(pre + row.body);
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string(row.out));
    }
}

// narrow #7792 fix-forward: such a local repeated in a loop does not leak
// (the object is owned by `w`; `t` only names it).
BEZEL_TEST(textwriter_value_7792, user_subclass_local_in_loop_no_leak_7_4) {
    auto r = runAotChecked(std::string("module M; ") + kSub +
        "class P public static method Main() var i := 0; "
        "while i < 200 do using var w := new W(); var t: TextWriter := w; "
        "if t is W then i := i + 1; end if; end while; "
        "Console.WriteLine(i); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// narrow #7792 fix-forward: every OTHER use of such a local would read it as
// a built-in `TextWriter` (a descriptor), so it stays a located compile error:
// a `TextWriter` receiver, a `TextWriter` parameter, a copy to another local,
// a reassignment, `??`, `as TextWriter`, comparison with `Console.Out`.
BEZEL_TEST(textwriter_value_7792, user_subclass_local_textwriter_uses_rejected_7_4) {
    const std::string pre = std::string("module M; ") + kSub;
    const char* bodies[] = {
        "class P public static method Main() var w := new W(); var t: TextWriter := w; "
        "t.Write(\"x\"); end Main; end P;",
        "class P static method E(u: TextWriter) u.WriteLine(\"x\"); end E; "
        "public static method Main() var w := new W(); var t: TextWriter := w; E(t); "
        "end Main; end P;",
        "class P public static method Main() var w := new W(); var t: TextWriter := w; "
        "var u: TextWriter := t; u.WriteLine(\"x\"); end Main; end P;",
        "class P public static method Main() var w := new W(); var t: TextWriter := w; "
        "t := Console.Out; t.WriteLine(\"x\"); end Main; end P;",
        "class P public static method Main() var w := new W(); var t: TextWriter? := w; "
        "var u := t ?? Console.Out; u.WriteLine(\"x\"); end Main; end P;",
        "class P public static method Main() var w := new W(); var t: TextWriter := w; "
        "var u := t as TextWriter; Console.WriteLine(u = null); end Main; end P;",
        "class P public static method Main() var w := new W(); var t: TextWriter := w; "
        "Console.WriteLine(t = Console.Out); end Main; end P;",
    };
    for (const char* b : bodies) {
        auto r = runAot(pre + b);
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kSubNeedle));
    }
}

// narrow #7792 fix-forward: a `TextWriter` that was never given a stream is a
// compile error, as for a user class (spec §7.9; §17.11 `default(T)`), not a
// write to descriptor 0 (the review's a17, a16, a09).
BEZEL_TEST(textwriter_value_7792, unassigned_textwriter_rejected_7_9) {
    struct Row { const char* src; const char* needle; };
    const Row rows[] = {
        {"module M; class P public static method Main() var w: TextWriter; "
         "Console.WriteLine(\"a\"); w.WriteLine(\"unassigned\"); Console.WriteLine(\"b\"); "
         "end Main; end P;",
         "read of unassigned local `w`"},
        {"module M; class H private w: TextWriter; public constructor() end constructor; "
         "public method Go() w.WriteLine(\"field-default\"); end Go; end H; "
         "class P public static method Main() var h := new H(); h.Go(); end Main; end P;",
         "constructor does not initialize non-nullable field `w`"},
        {"module M; class P public static method Main() "
         "var w: TextWriter := default(TextWriter); w.WriteLine(\"default-writer\"); "
         "end Main; end P;",
         "`default(T)` is a compile error for a non-nullable reference type"},
    };
    for (const Row& row : rows) {
        auto r = runAot(row.src);
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, row.needle));
    }
}

// Control: an assigned `TextWriter` local / field still compiles and writes.
BEZEL_TEST(textwriter_value_7792, assigned_textwriter_local_and_field_run_7_9) {
    auto r = runAotChecked(
        "module M; class H private w: TextWriter; "
        "public constructor() w := Console.Out; end constructor; "
        "public method Go() w.WriteLine(\"field\"); end Go; end H; "
        "class P public static method Main() var w: TextWriter; "
        "var b := true; if b then w := Console.Out; else w := Console.Error; end if; "
        "w.WriteLine(\"local\"); var h := new H(); h.Go(); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("local\nfield\n"));
}

// narrow #7792 second fix-forward (stdlib §7.4, spec §17.4, §19.3, v0.355): a
// type test or cast INTO the built-in `TextWriter` cannot produce a stream
// descriptor from an object, so every spelling is a located compile error.
// MEASURED on 225833d4 (BEFORE Issue #8256): the review's u05
// (`(new W() as TextWriter)!`) and u08 (`(o as TextWriter)!` over an `Object`)
// compiled with only the #2434 degrade warning and `t.WriteLine("hi")` wrote
// to descriptor 0 (stdin); `??`, a conditional arm, an interface carrier, a
// generic `x as TextWriter` and the inferred local did the same; `o is
// TextWriter` emitted nothing (not even the line); a typed `case` binding ran
// no arm at all.
//
// Issue #8256 SPLIT (the chain's own commit a37906fc6; spec §12.7, stdlib
// §7.4, spec §17.4 / §19.3). The `is` / `is not` FORMS over a compiler-known
// I/O class head are now ADMITTED, because a type test produces a `bool`
// (§12.7) and the runtime can now answer it. The `as` forms and the typed
// `case` binding keep the #7792 rejection: they must produce a VALUE of the
// target type, and a `TextWriter` value (the i32 stream descriptor, §17.4 /
// §19.3) has no representation for an object.
//
// ⇒ THE MUST-BE-REFUSED SET BELOW IS THE `as` / `case` ROWS ONLY. The two `is`
// / `is not` rows leave it and are asserted in their own MUST-ANSWER loop at
// the end of this case. The sibling test
// tests/sema/builtin_io_class_type_test_8256_cluster11_test.cpp is the other
// half of this same contract: it pins `Console.Out is TextWriter` -> `True`
// and `Console.Out is not W` -> `True`, and keeps the `as TextWriter`
// rejection green.
BEZEL_TEST(textwriter_value_7792, type_test_into_textwriter_rejected_17_4) {
    const std::string pre = std::string("module M; interface I end I; ") +
        "class W: TextWriter, I "
        "  public constructor() end constructor; "
        "  public override method Write(value: string) Console.Write(\"[\" + value + \"]\"); end Write; "
        "  public override method Write(value: char) Console.Write(\"<c>\"); end Write; "
        "  public override method Flush() end Flush; "
        "end W; "
        "class P public static method Main() var w := new W(); var o: Object := w; "
        "var i: I := w; var b := true; ";
    const char* post = " Console.WriteLine(\"end\"); end Main; end P;";
    const char* bodies[] = {
        // u05: a user subclass cast straight into `TextWriter`.
        "var t: TextWriter := (new W() as TextWriter)!; t.WriteLine(\"hi\");",
        // u08: out of an `Object` carrier.
        "var t: TextWriter := (o as TextWriter)!; t.WriteLine(\"hi\");",
        // `??` with a real stream on the right.
        "var t: TextWriter := (o as TextWriter) ?? Console.Out; t.WriteLine(\"hi\");",
        // A conditional arm.
        "var t: TextWriter := (if b then (o as TextWriter)! else Console.Out); "
        "t.WriteLine(\"hi\");",
        // Out of an interface carrier.
        "var t: TextWriter := (i as TextWriter)!; t.WriteLine(\"hi\");",
        // The inferred local, and the cast used directly as a receiver.
        "var t := (w as TextWriter)!; t.WriteLine(\"hi\");",
        "(o as TextWriter)!.WriteLine(\"hi\");",
        // `as TextWriter?` and a nullable local.
        "var t: TextWriter? := o as TextWriter?; Console.WriteLine(t = null);",
        // A typed `case` binding (ran no arm).
        "case o of var t: TextWriter => t.WriteLine(\"hi\"), _ => Console.WriteLine(\"no\") end;",
        // Even from a built-in stream (lowered to null: a NullReferenceException).
        "var t := Console.Out as TextWriter; t!.WriteLine(\"hi\");",
    };
    for (const char* b : bodies) {
        auto r = runAot(pre + b + post);
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
            r, "a type test or cast into the built-in `TextWriter`"));
    }
    // A generic `x as TextWriter` (the template body is the site).
    {
        auto r = runAot(std::string("module M; ") + kSub +
            "class G public static method F<T>(x: T): TextWriter? "
            "return x as TextWriter; end F; end G; "
            "class P public static method Main() var w := new W(); var o: Object := w; "
            "var t := G.F<Object>(o); t!.WriteLine(\"hi\"); end Main; end P;");
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
            r, "a type test or cast into the built-in `TextWriter`"));
    }
    // Issue #8256: the `is` / `is not` FORMS are ANSWERED, not refused (spec
    // §12.7: a type test produces a `bool`; stdlib §7.4: `TextWriter` is a
    // user-extensible class, so a `W` IS a `TextWriter`; stdlib §1
    // *Extensibility* names `TextWriter` among the heads that admit a
    // user-declared subclass). MEASURED on the chain's tip (a37906fc6) with
    // `bezelc`: `o is TextWriter` prints `True` (the `Object` carrier holds a
    // `W`), and `i is not TextWriter` prints `False` (the interface carrier
    // holds a `W` too, so it IS a `TextWriter` and the negated test is false).
    {
        struct Row { const char* body; const char* out; };
        const Row rows[] = {
            {"Console.WriteLine(o is TextWriter);", "True\nend\n"},
            {"Console.WriteLine(i is not TextWriter);", "False\nend\n"},
        };
        for (const Row& row : rows) {
            auto r = runAotChecked(pre + row.body + post);
            BEZEL_EXPECT_TRUE(r.ran);
            BEZEL_EXPECT_EQ(r.exitCode, 0);
            BEZEL_EXPECT_EQ(r.stdoutText, std::string(row.out));
        }
    }
}

// narrow #7792 second fix-forward: the OPERAND side. A built-in `TextWriter`
// value is a descriptor with no runtime type, so `tw as W` is a located
// compile error. MEASURED on 225833d4 (BEFORE Issue #8256):
// `Console.WriteLine(tw is W)` emitted nothing, `tw as W` degraded with the
// #2434 warning.
//
// Issue #8256 SPLIT (the chain's own commit a37906fc6; spec §12.7, stdlib
// §7.4, spec §17.4 / §19.3). The `is` / `is not` FORMS over a compiler-known
// I/O class head are now ADMITTED, because a type test produces a `bool`
// (§12.7) and the runtime can now answer it. The `as` forms and the typed
// `case` binding keep the #7792 rejection: they must produce a VALUE of the
// target type, and a `TextWriter` value (the i32 stream descriptor, §17.4 /
// §19.3) has no representation for an object.
//
// ⇒ THE MUST-BE-REFUSED SET BELOW IS THE `as` ROW ONLY. The `is` row leaves it
// and is asserted in its own MUST-ANSWER loop at the end of this case. The
// sibling test tests/sema/builtin_io_class_type_test_8256_cluster11_test.cpp
// is the other half of this same contract: it pins `Console.Out is TextWriter`
// -> `True` and `Console.Out is not W` -> `True`, and keeps the `as
// TextWriter` rejection green.
BEZEL_TEST(textwriter_value_7792, type_test_of_textwriter_value_rejected_17_4) {
    const std::string pre = std::string("module M; ") + kSub +
        "class P public static method H(tw: TextWriter) ";
    const char* post = " end H; public static method Main() H(Console.Out); end Main; end P;";
    const char* bodies[] = {
        "var u := tw as W; Console.WriteLine(u = null);",
    };
    for (const char* b : bodies) {
        auto r = runAot(pre + b + post);
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
            r, "a type test or cast of a built-in `TextWriter` value"));
    }
    // Issue #8256: the `is` / `is not` FORMS over a `TextWriter` VALUE answer
    // too (spec §12.7: a type test produces a `bool`; stdlib §7.4:
    // `TextWriter` is a user-extensible class). The answer is a fact about the
    // DECLARED type -- the standard stream is an instance of `TextWriter` and
    // of `Object` (§8.1) and of no other class (§12.7) -- so `tw is W` is
    // `False`. MEASURED on the chain's tip (a37906fc6) with `bezelc`.
    {
        struct Row { const char* body; const char* out; };
        const Row rows[] = {
            {"Console.WriteLine(tw is W);", "False\n"},
        };
        for (const Row& row : rows) {
            auto r = runAotChecked(pre + row.body + post);
            BEZEL_EXPECT_TRUE(r.ran);
            BEZEL_EXPECT_EQ(r.exitCode, 0);
            BEZEL_EXPECT_EQ(r.stdoutText, std::string(row.out));
        }
    }
}

// Controls for the two rows above: a `case` over a `TextWriter` with a user
// arm never matches (correct, and still admitted); a user-type test of an
// `Object` carrier runs.
BEZEL_TEST(textwriter_value_7792, type_test_controls_still_run_17_4) {
    const std::string pre = std::string("module M; ") + kSub;
    struct Row { const char* body; const char* out; };
    const Row rows[] = {
        {"class P static method H(tw: TextWriter) "
         "case tw of var x: W => Console.WriteLine(\"w\"), _ => Console.WriteLine(\"no\") end; "
         "end H; public static method Main() H(Console.Out); end Main; end P;",
         "no\n"},
        {"class P public static method Main() var w := new W(); var o: Object := w; "
         "Console.WriteLine(o is W); var b := o as W; b!.Write(\"o\"); Console.WriteLine(); "
         "end Main; end P;",
         "True\n[o]\n"},
    };
    for (const Row& row : rows) {
        auto r = runAotChecked(pre + row.body);
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string(row.out));
    }
}

// narrow #7792 second fix-forward: a local declared `TextWriter?` holding a
// user subclass keeps the object in the {i8, i32} carrier of a descriptor,
// so only its presence flag reads correctly. MEASURED on 225833d4 (the
// review's r09e r09f r09g q09d): `Console.WriteLine(t)` printed `0`, `t is W`
// was false, `$"{t}"` was empty. Every read but `= null` / `<> null` is now a
// located compile error.
BEZEL_TEST(textwriter_value_7792, nullable_subclass_local_reads_rejected_7_4) {
    const std::string pre = std::string("module M; ") + kSub +
        "class P public static method Main() var w := new W(); var t: TextWriter? := w; ";
    const char* post = " end Main; end P;";
    const char* bodies[] = {
        "Console.WriteLine(t);",                                  // r09e / q09d
        "if t is W then Console.WriteLine(\"isW\"); end;",          // r09f
        "Console.WriteLine($\"{t}\");",                           // r09g
        "var x := t as W; Console.WriteLine(x = null);",
        "var o: Object? := t; Console.WriteLine(o = null);",
        "var u: TextWriter? := w; Console.WriteLine(t = u);",
        "if t <> null then Console.WriteLine(t); end;",
    };
    for (const char* b : bodies) {
        auto r = runAot(pre + b + post);
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kSubNeedle));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
            r, "may only be compared with `null`"));
    }
}

// Control: its `= null` / `<> null` reads still run (the flag is sound), in a
// loop under the checked oracle.
BEZEL_TEST(textwriter_value_7792, nullable_subclass_local_null_test_runs_7_4) {
    auto r = runAotChecked(std::string("module M; ") + kSub +
        "class P public static method Main() var n := 0; var i := 0; "
        "while i < 100 do var t: TextWriter? := new W(); "
        "if t <> null then n := n + 1; end; if t = null then n := n - 1000; end; "
        "i := i + 1; end; Console.WriteLine(n); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// ---------------------------------------------------------------------------
// narrow #7792 (stdlib §2.1 / §7.4, spec §7.7, §20, §19.2, v0.355): every
// spelling that PRODUCES a `TextWriter` / `TextWriter?` value from a nullable
// one. Before this fix `TextWriter?` was not a member of codegen's
// nullable-VALUE family (it reached the `{i8, i32}` carrier only by accident)
// and a bare `Console.Out` / `Console.Error` operand carried no type, so
// `n ?? Console.Error` returned the RAW carrier: a null `n` wrote through
// DESCRIPTOR 0. MEASURED on 1021a84f with the fd-0 trap: `ce\n` landed in the
// file opened on fd 0, stderr stayed empty. Each row below asserts the stderr
// / stdout text and that fd 0 received nothing.
// ---------------------------------------------------------------------------

namespace {

std::string twMain(const std::string& body, const std::string& decls = "") {
    return "module M; " + decls + "class P public static method Main() " + body +
           " Console.WriteLine(\"end\"); end Main; end P;";
}

// The common oracle: the program compiled, exited 0, wrote `err` to standard
// error and `out` to standard output, and NOTHING through descriptor 0.
void expectStreams(const Fd0TrapOutcome& o, const std::string& out,
                   const std::string& err) {
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, out);
    BEZEL_EXPECT_EQ(o.stderrText, err);
    BEZEL_EXPECT_EQ(o.fd0Bytes, 0L);
}

}  // namespace

// z01: the review's repro -- a null `TextWriter?` local on the left of `??`,
// the result inferred into a `var`.
BEZEL_TEST(textwriter_value_7792, coalesce_null_left_var_local_2_1) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; var w := n ?? Console.Error; "
        "w.WriteLine(\"ce\");"));
    expectStreams(o, "end\n", "ce\n");
}

// z02: the `??` result used directly as the receiver.
BEZEL_TEST(textwriter_value_7792, coalesce_null_left_direct_receiver_2_1) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; (n ?? Console.Error).WriteLine(\"ce\");"));
    expectStreams(o, "end\n", "ce\n");
}

// z03: the result stored into a local DECLARED `TextWriter`.
BEZEL_TEST(textwriter_value_7792, coalesce_null_left_typed_local_2_1) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; var w: TextWriter := n ?? Console.Error; "
        "w.WriteLine(\"ce\");"));
    expectStreams(o, "end\n", "ce\n");
}

// z05: a nullable PARAMETER on the left, called with `null` and with a stream.
BEZEL_TEST(textwriter_value_7792, coalesce_null_left_parameter_2_1) {
    auto o = runWithFd0Trap(
        "module M; class P static method H(n: TextWriter?) "
        "var w := n ?? Console.Error; w.WriteLine(\"ce\"); end H; "
        "public static method Main() H(null); H(Console.Out); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    expectStreams(o, "ce\nend\n", "ce\n");
}

// z07: `?? Console.Out` -- the right operand is standard OUTPUT.
BEZEL_TEST(textwriter_value_7792, coalesce_null_left_out_right_2_1) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; var w := n ?? Console.Out; "
        "w.WriteLine(\"co\");"));
    expectStreams(o, "co\nend\n", "");
}

// z09: a nullable FIELD on the left.
BEZEL_TEST(textwriter_value_7792, coalesce_null_left_field_2_1) {
    auto o = runWithFd0Trap(
        "module M; class H public n: TextWriter?; "
        "public constructor() n := null; end constructor; end H; "
        "class P public static method Main() var h := new H(); "
        "(h.n ?? Console.Error).WriteLine(\"ce\"); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    expectStreams(o, "end\n", "ce\n");
}

// The `??` inside a loop: three `Write("x")` through the coalesced stream,
// then a newline through `Console.Error` directly.
BEZEL_TEST(textwriter_value_7792, coalesce_null_left_in_loop_2_1) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; for var i in 0..<3 do "
        "var w := n ?? Console.Error; w.Write(\"x\"); end for; "
        "Console.Error.WriteLine(\"\");"));
    expectStreams(o, "end\n", "xxx\n");
}

// The right operand's other shapes: a `null` (result stays `TextWriter?`), a
// nullable local, a `TextWriter` parameter. These were correct before; pinned
// against regression by the family change.
BEZEL_TEST(textwriter_value_7792, coalesce_right_operand_shapes_7_7) {
    auto o = runWithFd0Trap(
        "module M; class P static method G(n: TextWriter?, d: TextWriter) "
        "var w := n ?? d; w.WriteLine(\"ce\"); end G; "
        "public static method Main() var n: TextWriter? := null; "
        "var m := n ?? null; Console.WriteLine(m = null); "
        "var o: TextWriter? := Console.Error; var m2 := n ?? o; "
        "m2!.WriteLine(\"ce2\"); G(null, Console.Error); "
        "var n2: TextWriter? := null; var o2: TextWriter? := null; "
        "var w3 := n2 ?? o2 ?? Console.Error; w3.WriteLine(\"ce3\"); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    expectStreams(o, "True\nend\n", "ce2\nce\nce3\n");
}

// An `if` EXPRESSION with a nullable arm and a stream arm (spec §20: the result
// is `TextWriter?`). Was an IR-verifier abort (the arms had no common lowering).
BEZEL_TEST(textwriter_value_7792, if_expression_nullable_and_stream_arms_20) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; var flag := true; "
        "var w2 := (if flag then n else Console.Error); Console.WriteLine(w2 = null); "
        "var w3 := (if not flag then n else Console.Error); w3!.WriteLine(\"ce\");"));
    expectStreams(o, "True\nend\n", "ce\n");
}

// An `if` EXPRESSION whose arms are BOTH bare stream reads (§20): the result is
// a `TextWriter`. Was a silent drop (no type on either arm, nothing lowered).
BEZEL_TEST(textwriter_value_7792, if_expression_two_stream_arms_20) {
    auto o = runWithFd0Trap(twMain(
        "var flag := true; var w := (if flag then Console.Error else Console.Out); "
        "w.WriteLine(\"ce\"); var v := (if not flag then Console.Error else Console.Out); "
        "v.WriteLine(\"co\");"));
    expectStreams(o, "co\nend\n", "ce\n");
}

// A `case` EXPRESSION with a nullable arm and a stream arm (§19.2 / §20). Was
// an IR-verifier abort.
BEZEL_TEST(textwriter_value_7792, case_expression_arms_19_2) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; var k := 1; "
        "var w := (case k of 0 => n, _ => Console.Error); w!.WriteLine(\"ce\"); "
        "var o: TextWriter? := Console.Error; "
        "var w2 := (case k of 1 => o, _ => Console.Out); w2!.WriteLine(\"ce2\");"));
    expectStreams(o, "end\n", "ce\nce2\n");
}

// `default(TextWriter?)` is null (§7.1) and coalesces to the right operand.
BEZEL_TEST(textwriter_value_7792, default_nullable_coalesces_7_1) {
    auto o = runWithFd0Trap(twMain(
        "var d := default(TextWriter?); Console.WriteLine(d = null); "
        "var w := d ?? Console.Error; w.WriteLine(\"ce\");"));
    expectStreams(o, "True\nend\n", "ce\n");
}

// A `TextWriter?` RECORD field (positional), null and present.
BEZEL_TEST(textwriter_value_7792, record_field_nullable_9_1) {
    auto o = runWithFd0Trap(twMain(
        "var r := new R(null); var w := r.W ?? Console.Error; w.WriteLine(\"ce\"); "
        "var r2 := new R(Console.Error); r2.W!.WriteLine(\"ce2\");",
        "record R(W: TextWriter?); "));
    expectStreams(o, "end\n", "ce\nce2\n");
}

// A `TextWriter?` CLASS field set from a constructor parameter.
BEZEL_TEST(textwriter_value_7792, class_field_from_ctor_parameter_7_4) {
    auto o = runWithFd0Trap(
        "module M; class H public n: TextWriter?; "
        "public constructor(w: TextWriter?) n := w; end constructor; end H; "
        "class P public static method Main() var h := new H(null); "
        "(h.n ?? Console.Error).WriteLine(\"ce\"); "
        "var h2 := new H(Console.Error); h2.n!.WriteLine(\"ce2\"); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    expectStreams(o, "end\n", "ce\nce2\n");
}

// An `Array<TextWriter?>` element, null and assigned.
BEZEL_TEST(textwriter_value_7792, array_element_nullable_15_1) {
    auto o = runWithFd0Trap(twMain(
        "var a := new Array<TextWriter?>(2, null); "
        "var w := a[0] ?? Console.Error; w.WriteLine(\"ce\"); "
        "a[1] := Console.Error; a[1]!.WriteLine(\"ce2\");"));
    expectStreams(o, "end\n", "ce\nce2\n");
}

// A `Dictionary<string, TextWriter?>` value, null and assigned. Was a SEGFAULT
// of the compiled program.
BEZEL_TEST(textwriter_value_7792, dictionary_value_nullable_15_3) {
    auto o = runWithFd0Trap(twMain(
        "var d := new Dictionary<string, TextWriter?>(); d[\"a\"] := null; "
        "var w := d[\"a\"] ?? Console.Error; w.WriteLine(\"ce\"); "
        "d[\"b\"] := Console.Error; d[\"b\"]!.WriteLine(\"ce2\");"));
    expectStreams(o, "end\n", "ce\nce2\n");
}

// A generic class instantiated at `T = TextWriter?` and `T = TextWriter`.
BEZEL_TEST(textwriter_value_7792, generic_field_at_textwriter_14_1) {
    auto o = runWithFd0Trap(twMain(
        "var b := new Box<TextWriter?>(null); var w := b.v ?? Console.Error; "
        "w.WriteLine(\"ce\"); var b2 := new Box<TextWriter>(Console.Error); "
        "b2.v.WriteLine(\"ce2\");",
        "class Box<T> public v: T; public constructor(x: T) v := x; "
        "end constructor; end Box; "));
    expectStreams(o, "end\n", "ce\nce2\n");
}

// A lambda whose body is the `??` (declared `() -> TextWriter`). Was an
// IR-verifier abort.
BEZEL_TEST(textwriter_value_7792, lambda_returns_coalesce_27_1) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; "
        "var f: () -> TextWriter := lambda() => n ?? Console.Error; "
        "f().WriteLine(\"ce\");"));
    expectStreams(o, "end\n", "ce\n");
}

// An async method returning `Task<TextWriter>` from the `??`, awaited. Was a
// silent drop (the awaited value never reached the write).
BEZEL_TEST(textwriter_value_7792, async_returns_coalesce_30_1) {
    auto o = runWithFd0Trap(
        "module M; class P static async method A(n: TextWriter?): Task<TextWriter> "
        "return n ?? Console.Error; end A; "
        "public static async method Main(): Task<int> var w := await A(null); "
        "w.WriteLine(\"ce\"); Console.WriteLine(\"end\"); return 0; end Main; end P;");
    expectStreams(o, "end\n", "ce\n");
}

// An iterator yielding the `??` and then `Console.Out`.
BEZEL_TEST(textwriter_value_7792, iterator_yields_coalesce_16_1) {
    auto o = runWithFd0Trap(
        "module M; class P static method It(n: TextWriter?): IEnumerable<TextWriter> "
        "yield return n ?? Console.Error; yield return Console.Out; end It; "
        "public static method Main() for var w in It(null) do w.WriteLine(\"ce\"); "
        "end for; Console.WriteLine(\"end\"); end Main; end P;");
    expectStreams(o, "ce\nend\n", "ce\n");
}

// The `??` result at a RETURN, an ARGUMENT, a FIELD store, and a call result
// on the left.
BEZEL_TEST(textwriter_value_7792, coalesce_at_return_argument_field_store_7_7) {
    auto o = runWithFd0Trap(
        "module M; class H public w: TextWriter; "
        "public constructor(n: TextWriter?) w := n ?? Console.Error; end constructor; "
        "end H; class P "
        "static method R(n: TextWriter?): TextWriter return n ?? Console.Error; end R; "
        "static method Use(w: TextWriter) w.WriteLine(\"ce2\"); end Use; "
        "static method Pick(k: int): TextWriter? "
        "return (if k = 0 then null else Console.Error); end Pick; "
        "public static method Main() R(null).WriteLine(\"ce\"); "
        "var n: TextWriter? := null; Use(n ?? Console.Error); "
        "var h := new H(null); h.w.WriteLine(\"ce3\"); "
        "var w := Pick(0) ?? Console.Error; w.WriteLine(\"ce4\"); "
        "Pick(1)!.WriteLine(\"ce5\"); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    expectStreams(o, "end\n", "ce\nce2\nce3\nce4\nce5\n");
}

// A `TextWriter?`-returning method with `return null` on one path.
BEZEL_TEST(textwriter_value_7792, nullable_return_null_path_coalesces_7_7) {
    auto o = runWithFd0Trap(
        "module M; class P static method Pick(z: bool): TextWriter? "
        "if z then return null; end; return Console.Out; end Pick; "
        "public static method Main() var w := Pick(true) ?? Console.Error; "
        "w.WriteLine(\"ce\"); Pick(false)!.WriteLine(\"co\"); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    expectStreams(o, "co\nend\n", "ce\n");
}

// `?.` on a `TextWriter?` receiver (§7.5): skipped when null, writes when
// present.
BEZEL_TEST(textwriter_value_7792, null_conditional_write_7_5) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := null; n?.WriteLine(\"x\"); "
        "var o: TextWriter? := Console.Error; o?.WriteLine(\"ce\");"));
    expectStreams(o, "end\n", "ce\n");
}

// The controls the review found correct (z04, z06, z08, the narrowed read and
// `??=`), re-pinned under the fd-0 trap so the family change cannot regress
// them.
BEZEL_TEST(textwriter_value_7792, nullable_controls_under_fd0_trap_7_4) {
    auto o = runWithFd0Trap(twMain(
        "var n: TextWriter? := Console.Error; var w := n ?? Console.Out; "
        "w.WriteLine(\"ce\"); n!.WriteLine(\"ce2\"); "
        "var m: TextWriter? := null; "
        "var w2 := (if m = null then Console.Error else m!); w2.WriteLine(\"ce3\"); "
        "if n <> null then n.WriteLine(\"ce4\"); end; "
        "m ??= Console.Error; m!.WriteLine(\"ce5\"); "
        "var p: TextWriter? := Console.Out; p ??= Console.Error; p!.WriteLine(\"co\"); "
        "Console.WriteLine(n = null); Console.WriteLine(m <> null);"));
    expectStreams(o, "co\nFalse\nTrue\nend\n", "ce\nce2\nce3\nce4\nce5\n");
}

// ---------------------------------------------------------------------------
// narrow #7792, fourth fix-forward (stdlib §2.1 / §7.4; spec §14.1, §9.1,
// v0.355): a GENERIC class or record instantiated at `TextWriter?` /
// `TextWriter` where EVERY type argument is TextWriter-shaped. The built-in
// `TextWriter` was not a name codegen's `isBuiltinTypeRefName` knew, so to
// `isUnboundTemplateArg` the type-argument list `<TextWriter?>` read as a
// list of BARE TYPE PARAMETERS: `lowerNew` installed no substitution and
// allocated / constructed the ERASED `%class.Box = {ptr, i8, i32}` while every
// read of `b.v` GEP'd the specialized `{ptr, i8, {i8, i32}}`. MEASURED on
// 5907d0b2 with the fd-0 trap: `new Box<TextWriter?>(Console.Error)` then
// `b.v!.WriteLine("b01")` wrote 4 bytes through DESCRIPTOR 0 and stderr stayed
// empty (ASan: heap-buffer-overflow, an 8-byte read at offset 12); the generic
// record `G<T>(V: T)` lost the write; `Pair<TextWriter?, TextWriter>` sent one
// write to stdout and the other to fd 0. A mixed list (`Pair<int,
// TextWriter?>`) was correct because the `int` forced the substitution. Each
// row asserts the stdout AND stderr text and that fd 0 received nothing.
// ---------------------------------------------------------------------------

namespace {

const char* kGenDecls =
    "class Box<T> public v: T; public constructor(x: T) v := x; end constructor; "
    "public method Get(): T return v; end Get; "
    "public static method Of(x: T): owned Box<T> return new Box<T>(x); end Of; "
    "end Box; "
    "class Two<T> public a: T; public b: T; "
    "public constructor(x: T, y: T) a := x; b := y; end constructor; end Two; "
    "class Lab<T> public n: int; public v: T; "
    "public constructor(k: int, x: T) n := k; v := x; end constructor; end Lab; "
    "class Pair<A, B> public a: A; public b: B; "
    "public constructor(x: A, y: B) a := x; b := y; end constructor; end Pair; "
    "class Opt<T> public v: T?; public constructor(x: T?) v := x; end constructor; "
    "end Opt; "
    "public record G<T>(V: T); public record P2<A, B>(X: A, Y: B); ";

}  // namespace

// b01 / r14: the review's repro -- `new Box<TextWriter?>(Console.Error)`,
// read through `!`, through `??`, and after a `null` store.
BEZEL_TEST(textwriter_value_7792, generic_class_ctor_at_textwriter_nullable_14_1) {
    auto o = runWithFd0Trap(twMain(
        "var b := new Box<TextWriter?>(Console.Error); b.v!.WriteLine(\"b01\"); "
        "(b.v ?? Console.Out).WriteLine(\"r14\"); b.v := null; "
        "(b.v ?? Console.Out).WriteLine(\"r14b\"); Console.WriteLine(b.v = null);",
        kGenDecls));
    expectStreams(o, "r14b\nTrue\nend\n", "b01\nr14\n");
}

// b02 / b03 / b06 / b09 / b14: the constructed value held in a typed, a
// nullable-typed and an inferred local, `Console.Out` as the argument, and a
// read through the generic `Get(): T` method.
BEZEL_TEST(textwriter_value_7792, generic_class_ctor_locals_and_get_14_1) {
    auto o = runWithFd0Trap(twMain(
        "var t: Box<TextWriter?> := new Box<TextWriter?>(Console.Error); "
        "t.v!.WriteLine(\"b02\"); "
        "var n: Box<TextWriter?>? := new Box<TextWriter?>(Console.Error); "
        "n!.v!.WriteLine(\"b03\"); "
        "var u := new Box<TextWriter?>(Console.Out); (u.v ?? Console.Error).WriteLine(\"b09\"); "
        "var g := new Box<TextWriter?>(Console.Error); "
        "(g.Get() ?? Console.Out).WriteLine(\"b14\");",
        kGenDecls));
    expectStreams(o, "b09\nend\n", "b02\nb03\nb14\n");
}

// b10: the generic RECORD `G<T>(V: T)` (§9.1) at `TextWriter?`, with a
// stream and with `null`.
BEZEL_TEST(textwriter_value_7792, generic_record_ctor_at_textwriter_nullable_9_1) {
    auto o = runWithFd0Trap(twMain(
        "var g := new G<TextWriter?>(Console.Error); (g.V ?? Console.Out).WriteLine(\"b10\"); "
        "var h := new G<TextWriter?>(null); (h.V ?? Console.Out).WriteLine(\"b10b\"); "
        "Console.WriteLine(h.V = null);",
        kGenDecls));
    expectStreams(o, "b10b\nTrue\nend\n", "b10\n");
}

// b18 / b19: two `T` fields with one `null`, and an `int` field DECLARED
// before the `T` field.
BEZEL_TEST(textwriter_value_7792, generic_class_two_t_fields_and_int_first_14_1) {
    auto o = runWithFd0Trap(twMain(
        "var t := new Two<TextWriter?>(Console.Error, null); "
        "(t.a ?? Console.Out).WriteLine(\"b18\"); (t.b ?? Console.Out).WriteLine(\"b18b\"); "
        "var l := new Lab<TextWriter?>(1, Console.Error); "
        "(l.v ?? Console.Out).WriteLine(\"b19\"); Console.WriteLine(l.n);",
        kGenDecls));
    expectStreams(o, "b18b\n1\nend\n", "b18\nb19\n");
}

// b24: two type parameters, BOTH TextWriter-shaped -- `<TextWriter?,
// TextWriter>` and `<TextWriter?, TextWriter?>` -- as a class and as a record.
BEZEL_TEST(textwriter_value_7792, generic_pair_both_args_textwriter_shaped_14_1) {
    auto o = runWithFd0Trap(twMain(
        "var p := new Pair<TextWriter?, TextWriter>(Console.Error, Console.Out); "
        "(p.a ?? Console.Out).WriteLine(\"b24\"); p.b.WriteLine(\"b24b\"); "
        "var q := new Pair<TextWriter?, TextWriter?>(Console.Error, null); "
        "(q.a ?? Console.Out).WriteLine(\"s10\"); (q.b ?? Console.Out).WriteLine(\"s10b\"); "
        "var r := new P2<TextWriter?, TextWriter>(Console.Error, Console.Out); "
        "(r.X ?? Console.Out).WriteLine(\"s06\"); r.Y.WriteLine(\"s06b\"); "
        "var s := new P2<TextWriter, TextWriter>(Console.Error, Console.Out); "
        "s.X.WriteLine(\"s06c\"); s.Y.WriteLine(\"s06d\");",
        kGenDecls));
    expectStreams(o, "b24b\ns10b\ns06b\ns06d\nend\n", "b24\ns10\ns06\ns06c\n");
}

// The siblings of the `new` site that ask the same "is this type-argument
// list resolved?" question: a static method of the generic class, a generic
// static method instantiated at `TextWriter?`, an explicit `Id<TextWriter?>`,
// `default(TextWriter?)`, and a `T?` field at `T = TextWriter`.
BEZEL_TEST(textwriter_value_7792, generic_siblings_static_id_default_14_1) {
    auto o = runWithFd0Trap(
        std::string("module M; ") + kGenDecls +
        "class P static method Id<T>(x: T): T return x; end Id; "
        "static method Mk<T>(x: T): owned Box<T> return new Box<T>(x); end Mk; "
        "public static method Main() "
        "using var a := Box<TextWriter?>.Of(Console.Error); (a.v ?? Console.Out).WriteLine(\"s01\"); "
        "using var m := Mk<TextWriter?>(Console.Error); (m.v ?? Console.Out).WriteLine(\"s01b\"); "
        "var n: TextWriter? := Console.Error; (Id<TextWriter?>(n) ?? Console.Out).WriteLine(\"s02\"); "
        "Id<TextWriter>(Console.Out).WriteLine(\"s02b\"); "
        "var d: TextWriter? := default(TextWriter?); (d ?? Console.Error).WriteLine(\"s03\"); "
        "var o := new Opt<TextWriter>(Console.Error); (o.v ?? Console.Out).WriteLine(\"s14\"); "
        "var z := new Opt<TextWriter>(null); (z.v ?? Console.Error).WriteLine(\"s14b\"); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    expectStreams(o, "s02b\nend\n", "s01\ns01b\ns02\ns03\ns14\ns14b\n");
}

// Array-of-generic: an `Array<Box<TextWriter?>>` holding a local's box, and an
// `Array<G<TextWriter?>>` of generic records.
BEZEL_TEST(textwriter_value_7792, generic_array_of_box_and_record_14_1) {
    auto o = runWithFd0Trap(twMain(
        "var b0 := new Box<TextWriter?>(Console.Error); "
        "var arr := new Array<Box<TextWriter?>>(); arr.Add(b0); "
        "(arr[0].v ?? Console.Out).WriteLine(\"s04a\"); "
        "var ga := new Array<G<TextWriter?>>(); ga.Add(new G<TextWriter?>(Console.Error)); "
        "ga.Add(new G<TextWriter?>(null)); "
        "(ga[0].V ?? Console.Out).WriteLine(\"s04c\"); (ga[1].V ?? Console.Out).WriteLine(\"s04d\");",
        kGenDecls));
    expectStreams(o, "s04d\nend\n", "s04a\ns04c\n");
}

// 200 constructions of the class and the record in a loop (the ASan leg: an
// erased allocation read through the specialized layout is a
// heap-buffer-overflow on every iteration), then the controls `Box<int>`,
// `Box<string>` and `Box<int?>`, whose layouts must not change.
BEZEL_TEST(textwriter_value_7792, generic_ctor_loop_200_and_value_controls_14_1) {
    auto o = runWithFd0Trap(twMain(
        "var k := 0; for var i in 0..<200 do "
        "var b := new Box<TextWriter?>(Console.Error); "
        "var g := new G<TextWriter?>(Console.Error); "
        "if b.v <> null then k := k + 1; end; if g.V <> null then k := k + 1; end; "
        "end for; Console.WriteLine(k); "
        "var bi := new Box<int>(5); Console.WriteLine(bi.v); "
        "var bs := new Box<string>(\"str\"); Console.WriteLine(bs.v); "
        "var bn := new Box<int?>(7); Console.WriteLine(bn.v ?? 0); "
        "var gi := new G<int?>(9); Console.WriteLine(gi.V ?? 0);",
        kGenDecls));
    expectStreams(o, "400\n5\nstr\n7\n9\nend\n", "");
}

// Nested: a `Box<Box<TextWriter?>>` and a `Box<G<TextWriter?>>` whose inner
// box / record is read through a local. MEASURED on 5907d0b2: the inner
// `Box<TextWriter?>` was constructed at the erased layout, so the inner read
// wrote 3 bytes through descriptor 0 (`v1`) and the record's write was lost
// (`v7`). The CHAINED read `bb.v.v` of a nullable value built-in through two
// generic instantiations is a separate, pre-existing defect (base-identical
// for `Box<Box<Guid?>>`), tracked as #8269; it is not pinned here.
BEZEL_TEST(textwriter_value_7792, generic_nested_box_read_through_local_14_1) {
    auto o = runWithFd0Trap(twMain(
        "var bb := new Box<Box<TextWriter?>>(new Box<TextWriter?>(Console.Error)); "
        "var inner := bb.v; (inner.v ?? Console.Out).WriteLine(\"v1\"); "
        "var bg := new Box<G<TextWriter?>>(new G<TextWriter?>(Console.Error)); "
        "var ig := bg.v; (ig.V ?? Console.Out).WriteLine(\"v7\"); "
        "var bn := new Box<Box<TextWriter>>(new Box<TextWriter>(Console.Out)); "
        "var inn := bn.v; inn.v.WriteLine(\"v8\");",
        kGenDecls));
    expectStreams(o, "v8\nend\n", "v1\nv7\n");
}
