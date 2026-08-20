// #2013 (§12.9 type-test invariance / §2.8.3 rule 4 monomorphization / §17.4, v0.197) —
// the structural descriptor/monomorphization key (appendDescKey, via
// typeRefIdentityKey / descIdentityKey in LlvmBackend_Setup.cpp) must recurse
// into the generic arguments of a NULL-DECLARATION builtin generic type
// (`Array<T>`, `Dictionary<K,V>`, … — declaration intentionally null, per
// TypeResolution_Names §2.5/§29.3).
//
// THE DEFECT. The null-declaration branch of appendDescKey emitted only
// `"U" + headName` and did NOT recurse into `genericArgs`, so `Array<int>` and
// `Array<string>` produced the SAME key ("UArray"). The method-specialization
// cache (buildSpecializedMethodFn, Calls2.cpp) keys each instantiation on
// typeRefIdentityKey; with the collapsed key the two instantiations shared ONE
// specialized body — the second reused the first (the same class as #1947's
// self-nested collision and #1944's built-in-exception collision, here for the
// generic-ARGUMENT of a builtin). The runtime descriptor identity (descIdentityKey
// / instanceDescriptorFor) collapses the same way when such a type appears as a
// nested argument. This is the memory-unsafety precursor the fix closes.
//
// WHAT SURFACE THIS TEST REACHES. After #2001 (inline-array size) and #2006 (the
// plain `is`/`as` erasure guard), a null-decl builtin argument is NOT runtime-
// reified (`typeArgRuntimeReified` returns false for a null-declaration Named),
// so the plain `o is Array<int>` path is REJECTED at compile time and the runtime
// `is`/`as` descriptor for such a type never discriminates — the runtime-value
// surfaces are all erased for these types. The reachable, observable surface is
// the METHOD-SPECIALIZATION CACHE: two instantiations of one generic method that
// differ ONLY in a null-decl-builtin type argument must mint DISTINCT specialized
// functions. We lower the program with the real codegen (`emitModule`) and count
// the specialized functions the compiler emitted for the generic method (named
// `<Parent>$<Method>$spec`, LLVM-uniquified — a null-decl arg contributes no name
// suffix, so distinct-but-collapsed instantiations would collide onto one name).
//   * WITHOUT the fix: the two instantiations collapse -> ONE specialized function.
//   * WITH the fix:    distinct keys -> TWO specialized functions.
// The dedup cases pin the other direction: an IDENTICAL argument (used twice) must
// still share ONE specialization (the fix must split only on DIFFERING arguments).
//
// (A full end-to-end memory-unsafety could not be exhibited for these types with
// the current language surface — they carry no runtime identity token, cannot be
// `is`/`as`-tested after #2006, and a bare generic body cannot manipulate their
// element-typed internals; so the observable is the specialization-cache split
// itself, which is the exact code path the collapsed key corrupted. See the issue
// note: admitting the erased `is`/`as` cases is a separate follow-up requiring the
// `typeArgRuntimeReified` update, out of scope for this appendDescKey fix.)

#include <string>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

// Lower `source` through the real front-end + `emitModule`, then count the
// functions whose name begins with `prefix` (the specialized-method-function
// naming from buildSpecializedMethodFn, e.g. `C$Tag$spec`). Returns -1 on any
// front-end / codegen failure (so a broken program never masquerades as a count).
int countFunctionsWithPrefix(const std::string& source,
                             const std::string& prefix) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -1;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -1;
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    if (!module) return -1;
    int n = 0;
    for (const llvm::Function& f : *module)
        if (f.getName().str().rfind(prefix, 0) == 0) ++n;  // starts-with prefix
    return n;
}

// A method-level generic `Tag<T>` with a BLOCK body (so it is monomorphized via
// buildSpecializedMethodFn — an expression body is inlined per call site and
// never exercises the cache) whose body references `T` (`case o of T`) so the
// instantiation is genuinely specialized. `bodyMain` supplies the two (or more)
// call sites. The specialized functions are named `C$Tag$spec[...]`.
std::string prog(const std::string& mainBody) {
    return "module M;\n"
           "class Foo end Foo;\n"
           "class C\n"
           "  public static method Tag<T>(o: Object): int\n"
           "    return (case o of T => 1, _ => 0);\n"
           "  end Tag;\n"
           "  public static method Main(): int\n" +
           mainBody +
           "  end Main;\n"
           "end C;\n";
}

constexpr const char* kPrefix = "C$Tag$spec";

}  // namespace

// ---- HEADLINE: distinct null-decl-builtin args -> distinct specializations -----

// `Array<int>` and `Array<string>` differ ONLY in the (null-declaration builtin
// `Array`'s) type argument. They MUST mint two distinct specialized bodies.
// WITHOUT the fix appendDescKey collapses both onto the key "UArray", so the cache
// returns ONE function (count == 1) — the collapse. WITH the fix the keys differ
// and the compiler emits TWO (count == 2). This is the fail-without / pass-with.
BEZEL_TEST(codegen_2013, array_int_vs_string_distinct_specializations) {
    int n = countFunctionsWithPrefix(prog(
        "    using var ai := new Array<int>();\n"
        "    var o: Object := ai;\n"
        "    var a := Tag<Array<int>>(o);\n"
        "    var b := Tag<Array<string>>(o);\n"
        "    return a + b;\n"), kPrefix);
    BEZEL_EXPECT_EQ(n, 2);
}

// ---- DEDUP PRESERVED: an identical argument still shares one specialization ----

// `Array<int>` used at TWO call sites is ONE type: the two calls must share ONE
// specialized body (the fix must split only on a DIFFERING argument, never on a
// repeated identical one). count == 1 both before and after the fix — a guard
// against over-splitting.
BEZEL_TEST(codegen_2013, array_int_twice_shares_one_specialization) {
    int n = countFunctionsWithPrefix(prog(
        "    using var ai := new Array<int>();\n"
        "    var o: Object := ai;\n"
        "    var a := Tag<Array<int>>(o);\n"
        "    var b := Tag<Array<int>>(o);\n"
        "    return a + b;\n"), kPrefix);
    BEZEL_EXPECT_EQ(n, 1);
}

// ---- OTHER null-decl builtins / nesting: split where args differ ---------------

// A multi-parameter builtin (`Dictionary<K,V>`): instantiations differing only in
// the SECOND argument (`Dictionary<int,string>` vs `Dictionary<int,long>`) must
// split. Without the fix both collapse to "UDictionary" -> ONE. With it -> TWO.
BEZEL_TEST(codegen_2013, dictionary_differing_value_arg_distinct) {
    int n = countFunctionsWithPrefix(prog(
        "    using var d := new Dictionary<int, string>();\n"
        "    var o: Object := d;\n"
        "    var a := Tag<Dictionary<int, string>>(o);\n"
        "    var b := Tag<Dictionary<int, long>>(o);\n"
        "    return a + b;\n"), kPrefix);
    BEZEL_EXPECT_EQ(n, 2);
}

// The same `Dictionary<int,string>` twice still shares ONE (dedup, multi-arg).
BEZEL_TEST(codegen_2013, dictionary_identical_args_shares_one) {
    int n = countFunctionsWithPrefix(prog(
        "    using var d := new Dictionary<int, string>();\n"
        "    var o: Object := d;\n"
        "    var a := Tag<Dictionary<int, string>>(o);\n"
        "    var b := Tag<Dictionary<int, string>>(o);\n"
        "    return a + b;\n"), kPrefix);
    BEZEL_EXPECT_EQ(n, 1);
}

// NESTED: `Array<Array<int>>` vs `Array<Array<string>>` — the differing argument
// is buried one level deeper inside another null-decl builtin. The recursion must
// reach it: without the fix BOTH collapse to "UArray" at the outer level (the
// inner arg is never visited) -> ONE; with it the key recurses through both
// levels -> TWO. Pins that the fix recurses (not just a one-level head compare).
BEZEL_TEST(codegen_2013, nested_array_of_array_distinct) {
    int n = countFunctionsWithPrefix(prog(
        "    using var ai := new Array<int>();\n"
        "    var o: Object := ai;\n"
        "    var a := Tag<Array<Array<int>>>(o);\n"
        "    var b := Tag<Array<Array<string>>>(o);\n"
        "    return a + b;\n"), kPrefix);
    BEZEL_EXPECT_EQ(n, 2);
}

// A primitive arg vs a class arg (`Array<int>` vs `Array<Foo>`): the argument
// kinds differ (Primitive vs resolved-Named), and both must be distinguished from
// each other under the null-decl `Array` head. count == 2.
BEZEL_TEST(codegen_2013, array_primitive_vs_class_arg_distinct) {
    int n = countFunctionsWithPrefix(prog(
        "    using var ai := new Array<int>();\n"
        "    var o: Object := ai;\n"
        "    var a := Tag<Array<int>>(o);\n"
        "    var b := Tag<Array<Foo>>(o);\n"
        "    return a + b;\n"), kPrefix);
    BEZEL_EXPECT_EQ(n, 2);
}

// ---- REGRESSION: both instantiations compile, link, and run clean --------------

// A module that uses BOTH `Array<int>` and `Array<string>` with real method calls,
// plus both `Tag` instantiations, must compile, link (the two same-named internal
// specializations are LLVM-uniquified — no duplicate-symbol), and run to the
// spec-stated value with no codegen crash. Count(2)=ai=2, asx=1, tags both 0
// (arrays carry no runtime identity, so `case o of Array<...>` never matches — an
// erased-but-sound outcome): 2*100 + 1*10 + 0 = 210.
BEZEL_TEST(codegen_2013, both_instantiations_compile_and_run_clean) {
    auto r = runAot(prog(
        "    using var ai := new Array<int>();\n"
        "    ai.Add(10); ai.Add(20);\n"
        "    using var asx := new Array<string>();\n"
        "    asx.Add(\"x\");\n"
        "    var oi: Object := ai;\n"
        "    var r1 := Tag<Array<int>>(oi);\n"
        "    var r2 := Tag<Array<string>>(oi);\n"
        "    return ai.Count * 100 + asx.Count * 10 + (r1 + r2);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 210);
}
