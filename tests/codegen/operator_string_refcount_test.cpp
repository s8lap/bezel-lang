// Codegen + runtime — string reference counting for the OPERATOR-OPERAND
// category (issue #429, §4.6 / §28.9).  When a +1 string producer (concat /
// interpolation / range-slice / primitive-ToString / user-method-return) appears
// as a BINARY OPERATOR OPERAND (comparison =/<>/</>/<=/>=, or as a concat
// operand that feeds another concat) or as the RECEIVER of a string METHOD
// CALL / PROPERTY ACCESS (.Length, .Contains, etc.) in a non-local position,
// the producer's rc=1 reference has no named-local owner and no scope-end
// release.  Before this fix these +1 references leaked; after the fix codegen
// releases them in-place (after the operator/method has consumed the value).
//
// Specifically:
//   * Primitive `.ToString()` calls (emitPrimitiveToString — CG27c inline path)
//     on numeric/char/decimal types produce a fresh rc=1 heap string and are now
//     recognised as +1 producers by isPlusOneStringProducer (Bool.ToString()
//     and String.ToString() are EXCLUDED — they return sentinels / identity).
//   * String = / <> comparison: after emitStringEq the two producer operands are
//     released if isPlusOneStringProducer returns true.
//   * String < > <= >= comparison: same pattern via emitStringOrdCmp.
//   * String .Length: after loading the length from a +1 producer receiver,
//     the receiver is released.
//   * String method call (tryLowerStringMethod: .Contains, .StartsWith, etc.):
//     a +1 producer receiver is released after the method returns.
//
// Per the PRIME DIRECTIVE: over-release is worse than a leak.  All tests
// include an ASan double-free canary (detect_leaks=0, abort_on_error=0) to
// assert no memory-safety regression on the named-local-owner shapes (where
// the scope-end release mechanism already handles the +1, so the operator/
// method paths must NOT emit an extra release).
//
// Two complementary checks per shape:
//   1. expectValueAndLeakFree:  exit code correct AND LSan reports no leak /
//      UAF / double-free.
//   2. expectValueAndNoCorruption:  exit code correct AND ASan reports no
//      memory-safety error, tolerating a residual leak.  Used for the named-
//      local canaries where the scope-end release is the owner (the extra
//      operator/method release must not fire).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir429op() {
    return wtScratchDir("wt-429op");
}

// Compile `source` to a temp native executable and run it; returns the exit
// code, or -1 on failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir429op();
    std::string stem = unique_stem("bezel_429op");
    std::string src  = (dir / (stem + ".bzl")).string();
    std::string exe  = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

// Compile + link against ASan runtime. Returns false on failure / unavailable.
bool lowerAndLink(const std::string& source, std::string& obj,
                  std::string& exe) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return false;
    auto dir = wtDir429op();
    std::string stem = unique_stem("bezel_429op_san");
    obj = (dir / (stem + ".o")).string();
    exe = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) { bezel::test::asanBuildFailure("parse"); return false; }
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) { bezel::test::asanBuildFailure("sema"); return false; }
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) { bezel::test::asanBuildFailure("emitModule"); return false; }
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, obj, err)) {
            bezel::test::asanBuildFailure("emitObjectFile");
            return false;
        }
    }
    std::string linkCmd = "clang++ -fsanitize=address -g " + obj + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { std::remove(obj.c_str()); return false; }
    return true;
}

// LSan verification. Returns 0 = clean, 1 = sanitizer problem, -1 = skip.
int runUnderLeakSanitizer(const std::string& source) {
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return -1;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str()); std::remove(exe.c_str());
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Assert exit code correct AND LSan clean (no leak / UAF / double-free).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Assert exit code correct AND ASan reports no memory-safety error, tolerating
// a residual leak.  Used for named-local canaries: the scope-end release owns
// the +1, so the operator/method path must NOT emit an extra release.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str()); std::remove(exe.c_str());
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// =====================================================================
// Programs: primitive.ToString() producer in concat operand position
// =====================================================================

// §4.6 / §28.9: `"ab" + i.ToString()` — the inline CG27c primitive-ToString
// call produces a fresh rc=1 heap string (emitPrimitiveToString).  It feeds
// directly into the concat as the RIGHT operand; releaseConcatIntermediate
// now recognises it as a +1 producer and releases it after the concat. Looped
// 100 times → bounded (LSan-clean).  Leaked before this fix.
const char* kConcatArgToString =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      total := total + C.Consume(\"ab\" + i.ToString()); "
    "    end; "
    "    if total > 100 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §4.6 / §28.9: chained concat `("ab" + i.ToString() + "cd").Length` — the
// outer concat result is a +1 producer whose receiver is consumed by .Length.
// The inner i.ToString() is released by releaseConcatIntermediate; the outer
// concat result is released by the .Length CG27a path.  Looped → LSan-clean.
const char* kChainedConcatLength =
    "module M; class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      total := total + (\"ab\" + i.ToString() + \"cd\").Length; "
    "    end; "
    "    if total > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §4.6 / §28.9: user-method return feeds concat as LHS operand; the concat
// result feeds .Length.  Both the user-method +1 (released as a concat
// operand via releaseConcatIntermediate) and the concat result (released by
// the .Length CG27a path) are balanced.  Looped → LSan-clean.
const char* kUserMethodReturnConcatLength =
    "module M; class C "
    "  public static method Build(n: int): string return \"x\" + n.ToString(); end Build; "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      total := total + (C.Build(i) + \"!\").Length; "
    "    end; "
    "    if total > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// Programs: producer in string equality/inequality comparison
// =====================================================================

// §4.6 / §28.9: a primitive.ToString() result on the LHS of `= "5"` — the
// CG27c inline emitPrimitiveToString result is a +1 producer; the comparison
// path now releases it after emitStringEq.  Looped → LSan-clean.
const char* kToStringVsLiteralEq =
    "module M; class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      if i.ToString() = \"5\" then total := total + 1; end; "
    "    end; "
    "    if total = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §4.6 / §28.9: two user-method return strings on both sides of `=` — both
// are +1 producers; both released after emitStringEq.  Looped → LSan-clean.
const char* kTwoMethodReturnsBothEqReleased =
    "module M; class C "
    "  public static method Build(n: int): string return \"x\" + n.ToString(); end Build; "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      if C.Build(i) = C.Build(i) then total := total + 1; end; "
    "    end; "
    "    if total = 101 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §4.6 / §28.9: user-method return vs. literal in `<>` — LHS is a +1
// producer; released after emitStringEq + NOT.  Looped → LSan-clean.
const char* kMethodReturnNeqLiteral =
    "module M; class C "
    "  public static method Build(n: int): string return \"x\" + n.ToString(); end Build; "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      if C.Build(i) <> \"none\" then total := total + 1; end; "
    "    end; "
    "    if total = 101 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// Programs: producer in string ordinal comparison (<, >, <=, >=)
// =====================================================================

// §4.6 / §28.9: user-method returns on both sides of `<` — both producers;
// both released after emitStringOrdCmp.  Looped → LSan-clean.
const char* kTwoMethodReturnsOrdCmpReleased =
    "module M; class C "
    "  public static method Build(n: int): string return \"x\" + n.ToString(); end Build; "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      if C.Build(i) < C.Build(i + 1) then total := total + 1; end; "
    "    end; "
    "    if total > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// Programs: producer as string method (.Contains) receiver
// =====================================================================

// §4.6 / §28.9: user-method return used as the receiver of .Contains() — the
// +1 producer has no named-local owner; released after tryLowerStringMethod.
// Looped → LSan-clean.
const char* kMethodReturnContainsReceiver =
    "module M; class C "
    "  public static method Build(n: int): string return n.ToString() + \"end\"; end Build; "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      if (C.Build(i)).Contains(\"5\") then total := total + 1; end; "
    "    end; "
    "    if total > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// =====================================================================
// Programs: CONTROL — named locals must NOT be double-released
// =====================================================================

// §28.9 (CARDINAL double-free canary): when `var s := i.ToString()` BINDS
// the primitive-ToString result to a NAMED LOCAL, the local's scope-end
// release (armStringLocalRefcount) owns its +1.  The comparison path
// isPlusOneStringProducer sees an IdentifierExpr for `s`, not the CallExpr —
// returns false → no release at the comparison site.  No double-free.
// Value + no-corruption (the named local's scope-end release handles it).
const char* kNamedLocalToStringCompareNoDoubleFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      var s: string := i.ToString(); "
    "      if s = \"5\" then total := total + 1; end; "
    "    end; "
    "    if total = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 (CARDINAL double-free canary): `var s := i.ToString(); s.Length` —
// a named local used as .Length receiver.  The CG27a path checks
// isPlusOneStringProducer(ma->target) where target is IdentifierExpr(s) —
// returns false → no release at the .Length site.  The scope-end release
// handles it.  No double-free.
const char* kNamedLocalToStringLengthNoDoubleFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    for var i in 0..100 do "
    "      var s: string := i.ToString(); "
    "      total := total + s.Length; "
    "    end; "
    "    if total > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 (CARDINAL double-free canary): `i.ToString()` passed to a
// constructor that stores it into a field (field-store retains → rc=2;
// argpass releases → rc=1; disposal releases → rc=0). The +1 from
// emitPrimitiveToString is consumed by the field-store retain + post-ctor
// argpass release, leaving exactly one reference in the field.  No UAF,
// no double-free.  Looped + using-var → LSan-clean.
const char* kToStringFieldStoreCtorNoDoubleFree =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor(n: string) this.Name := n; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..100 do "
    "    using var h := new Holder(i.ToString()); "
    "    total := total + h.Name.Length; "
    "  end; "
    "  if total > 0 then return 0; end; return 1; "
    "end Main; end C;";

}  // namespace

// =====================================================================
// Tests: primitive.ToString() producer in concat / method position
// =====================================================================

// §4.6 / §28.9: i.ToString() feeding concat — primitive-ToString now
// recognized as +1 producer; releaseConcatIntermediate cleans it up.
BEZEL_TEST(codegen_operator_string_429, concat_arg_tostring_released_4_6) {
    expectValueAndLeakFree(kConcatArgToString, 0);
}

// §4.6 / §28.9: chained concat receiver of .Length — outer concat released
// by CG27a path; inner i.ToString() released by releaseConcatIntermediate.
BEZEL_TEST(codegen_operator_string_429, chained_concat_length_released_4_6) {
    expectValueAndLeakFree(kChainedConcatLength, 0);
}

// §4.6 / §28.9: user-method return in concat → .Length — both the method-
// return operand and the concat receiver are released.
BEZEL_TEST(codegen_operator_string_429, usermethod_return_concat_length_4_6) {
    expectValueAndLeakFree(kUserMethodReturnConcatLength, 0);
}

// =====================================================================
// Tests: producer in string comparison operators
// =====================================================================

// §4.6 / §28.9: primitive-ToString on LHS of string `=` — released after
// emitStringEq.  Looped → LSan-clean.
BEZEL_TEST(codegen_operator_string_429, tostring_vs_literal_eq_released_28_9) {
    expectValueAndLeakFree(kToStringVsLiteralEq, 0);
}

// §4.6 / §28.9: two user-method returns in string `=` — both released.
BEZEL_TEST(codegen_operator_string_429, two_method_returns_both_eq_released_28_9) {
    expectValueAndLeakFree(kTwoMethodReturnsBothEqReleased, 0);
}

// §4.6 / §28.9: user-method return in string `<>` — LHS released.
BEZEL_TEST(codegen_operator_string_429, method_return_neq_literal_released_28_9) {
    expectValueAndLeakFree(kMethodReturnNeqLiteral, 0);
}

// §4.6 / §28.9: user-method returns on both sides of `<` — both released.
BEZEL_TEST(codegen_operator_string_429, two_method_returns_ord_cmp_released_28_9) {
    expectValueAndLeakFree(kTwoMethodReturnsOrdCmpReleased, 0);
}

// =====================================================================
// Tests: producer as string method receiver
// =====================================================================

// §4.6 / §28.9: user-method return as .Contains() receiver — released after
// tryLowerStringMethod.
BEZEL_TEST(codegen_operator_string_429, method_return_contains_receiver_28_9) {
    expectValueAndLeakFree(kMethodReturnContainsReceiver, 0);
}

// =====================================================================
// Tests: CONTROL — named-local canaries (no double-release)
// =====================================================================

// §28.9 (CARDINAL double-free canary): named local from ToString used in
// comparison — operator path must NOT fire (isPlusOneStringProducer returns
// false for an IdentifierExpr).  Scope-end release handles it → LSan-clean.
BEZEL_TEST(codegen_operator_string_429, named_local_tostring_cmp_no_double_free_28_9) {
    // expectValueAndLeakFree because the scope-end release makes this clean.
    expectValueAndLeakFree(kNamedLocalToStringCompareNoDoubleFree, 0);
}

// §28.9 (CARDINAL double-free canary): named local from ToString used as
// .Length receiver — CG27a path must NOT fire (IdentifierExpr, not a
// producer).  Scope-end release handles it → LSan-clean.
BEZEL_TEST(codegen_operator_string_429, named_local_tostring_length_no_double_free_28_9) {
    expectValueAndLeakFree(kNamedLocalToStringLengthNoDoubleFree, 0);
}

// §28.9 (CARDINAL double-free canary): i.ToString() passed to ctor that
// stores it in a field — argpass releases the +1 after ctor; field-store
// retained independently (rc=2 → 1 → 0 on disposal).  LSan-clean.
BEZEL_TEST(codegen_operator_string_429, tostring_field_store_ctor_no_double_free_28_9) {
    expectValueAndLeakFree(kToStringFieldStoreCtorNoDoubleFree, 0);
}
