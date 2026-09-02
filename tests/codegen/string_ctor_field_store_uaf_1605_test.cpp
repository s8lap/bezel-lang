// Codegen — a `string` primary-constructor argument stored into a class's
// positional-param field slot must take an INDEPENDENT +1 (retain-or-transfer,
// mirroring lowerStringFieldStore / emitOwnFieldInit's discipline for any other
// string field store), not merely alias the caller's reference (§4.6 / §28.9,
// issue #1605).
//
// Root cause: a class with a primary constructor (`class Box(v: string) ...`)
// gets a compiler-generated FieldSymbol for `v` (FieldSymbol::posParam != null)
// so that field initializers like `s: string := v;` can read the parameter's
// value via implicitFieldAddr — see §11.1 / §22.10. That hidden field slot is
// a genuine class field: it is walked and released by the generated
// `DisposeCore` exactly like any user-declared string field (regardless of
// whether the parameter is also read from a body position — §22.10's
// "captured" distinction governs `owned` reference fields, not this string
// slot's disposal). Two call sites populate it with a bare
// `builder_.CreateStore` and NO retain:
//   * lowerNew's "all-positional" constructor-argument loop
//     (LlvmBackend_Exprs2.cpp) — `new Box(s)`.
//   * emitChainCall's `this(...)`-to-primary-ctor branch
//     (LlvmBackend_Setup.cpp, the #1087 mechanism) — an additional
//     constructor delegating to the primary via `this(p)`.
// Without the retain, the hidden field ALIASES the caller's argument with no
// reference of its own. If the caller later releases that same reference
// (e.g. a `+=` release-old on the local that was passed in), the field
// dangles; the owner's generated `Dispose()` then reads/frees already-freed
// memory — a heap-use-after-free (and, since DisposeCore also releases the
// class's OWN declared field which correctly retained a copy, this is a
// distinct reference from that one — fixing the gap must not double-release
// the single string object either).
//
// Three checks per repro (mirroring string_accumulator_leak_1486_test.cpp /
// string_producer_leak_1481_1482_test.cpp):
//   * a JIT run: the program produces the spec-correct exit code (retaining
//     the hidden field must not perturb the field-initializer / disposal
//     values).
//   * a native AddressSanitizer run (load-bearing for the UAF fix): before the
//     fix ASan reports heap-use-after-free in bz_string_release (called from
//     the generated DisposeCore); after the fix it is clean.
//   * the same native run also proves dispose-once / no-double-free: if the
//     retain were applied WITHOUT giving the hidden field its own +1 (e.g. by
//     accident double-counting the class's own declared field's reference),
//     ASan would report a double-free / heap-corruption at the same
//     DisposeCore call instead of running clean.
//
// LeakSanitizer is intentionally OFF (ASAN_OPTIONS detect_leaks=0) in the
// native run below: a string local passed as a constructor argument is
// classified as ESCAPING by the local-string escape walk (LlvmBackend_Setup.cpp
// / lowerStringLocalReassign's doc comment), so a later `+=` reassignment of
// that SAME local intentionally leaves its final value unreleased at scope
// end — "the escape walk marks a reassigned local as escaped, so its final
// value is NOT auto-released at scope end and simply leaks (safe — never a
// UAF, never a double-free)". That is a pre-existing, documented,
// leak-over-UAF tradeoff orthogonal to #1605 (which is about the hidden
// FIELD's own reference, not the caller-local's) — asserting LeakSanitizer
// clean here would fail on that unrelated, already-accepted leak. Heap
// corruption (use-after-free, double-free, heap-buffer-overflow) is still
// fully caught with detect_leaks=0: that class of ASan error is independent
// of the separate LeakSanitizer-at-exit pass this disables.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Lower `source` to an object file, link against an ASan-instrumented
// runtime, run under AddressSanitizer. Returns 0 (clean), 1 (sanitizer
// reported an error — UAF, double-free, or a leak), or -1 (toolchain
// unavailable → SKIP). Mirrors string_accumulator_leak_1486_test.cpp /
// string_producer_leak_1481_1482_test.cpp's runUnderLeakSanitizer.
int runUnderAsan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_1605_asan");
}

void expectRunsAndAsanClean(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int asan = runUnderAsan(source);
    if (asan == -1) return;    // toolchain unavailable: skip the ASan leg
    BEZEL_EXPECT_EQ(asan, 0);  // 0 == ASan clean (no UAF, no double-free)
}

// ── §4.6 / §28.9 issue #1605's own repro: a primary-ctor STRING argument
//    (`new Box(s)`) stored into `s`'s field initializer via the hidden
//    positional-param slot, then the caller releases its OWN reference to the
//    same string via `+=`'s release-old. Pre-fix: ASan heap-use-after-free in
//    bz_string_release <- bz$Box$DisposeCore (the hidden slot dangled after
//    the caller's release). s="ab" (len 2), b.s="a" (len 1); (2+1) mod 256=3.
const char* kCtorArgRetainedAcrossCallerRelease_1605 =
    "module M; class Box(v: string) public s: string := v; end Box; "
    "class C public static method Main(): int "
    "  var s := \"\" + \"a\"; var b := new Box(s); s += \"b\"; "
    "  var r := (s.Length + b.s.Length) mod 256; _ := b; return r; "
    "end Main; end C;";
BEZEL_TEST(string_ctor_field_store_uaf_1605, positional_ctor_arg_retained_no_uaf) {
    expectRunsAndAsanClean(kCtorArgRetainedAcrossCallerRelease_1605, 3);
}

// ── The issue's own "contrast" repro — no `+=`, so the caller's local is
//    never early-released; this was already clean before the fix (both
//    references still alive at Dispose). Kept as a regression guard: the
//    added retain must not perturb this already-correct path or double-free
//    it at scope end. b.s="a" (len 1); 1 mod 256 = 1.
const char* kCtorArgNoEarlyRelease_1605 =
    "module M; class Box(v: string) public s: string := v; end Box; "
    "class C public static method Main(): int "
    "  var s := \"\" + \"a\"; var b := new Box(s); "
    "  var r := b.s.Length mod 256; _ := b; return r; "
    "end Main; end C;";
BEZEL_TEST(string_ctor_field_store_uaf_1605, positional_ctor_arg_no_early_release_still_clean) {
    expectRunsAndAsanClean(kCtorArgNoEarlyRelease_1605, 1);
}

// ── §22.10 / #1087's `this(...)`-to-primary-ctor delegation shares the exact
//    same hidden-field store site (emitChainCall, LlvmBackend_Setup.cpp), with
//    its own bare `CreateStore` needing the identical retain. An ADDITIONAL
//    constructor of different arity (string, int) avoids ambiguity with the
//    primary (string) ctor and delegates via `this(p)`; Main separately
//    releases its own `p` via `+=` after construction. p="ab" (len 2),
//    b.s="a" (len 1); (2+1) mod 256 = 3.
const char* kThisChainCtorArgRetainedAcrossCallerRelease_1605 =
    "module M; "
    "class Box(v: string) "
    "  public s: string := v; "
    "  public constructor(p: string, extra: int) this(p); end constructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  var p := \"\" + \"a\"; var b := new Box(p, 0); p += \"b\"; "
    "  var r := (p.Length + b.s.Length) mod 256; _ := b; return r; "
    "end Main; end C;";
BEZEL_TEST(string_ctor_field_store_uaf_1605, this_chain_ctor_arg_retained_no_uaf) {
    expectRunsAndAsanClean(kThisChainCtorArgRetainedAcrossCallerRelease_1605, 3);
}

}  // namespace
