// Codegen — a static class (§11.7) or an enum (§10.4/A.4) may declare a
// `static owned readonly` field (or a static owning-inline-array field), and
// that field participates in §22.13 program-exit disposal exactly as on any
// other type (issues #1557, #1564).
//
// §11.7 (v0.181): "Static `owned` fields are permitted … lifting an
// unrecorded leftover of the v0.55 ban: `static owned readonly` fields and
// static owning-inline-array fields participate in §22.13's program-exit
// disposal exactly as on any other type … that mechanism is runtime-driven
// and independent of instance `Dispose()`." Before the fix, sema rejected
// this with "static class `Globals` may not declare an owned field `Held`
// (§11.7)" (src/sema/TypeResolution_Checks.cpp, checkStaticClassMember).
//
// §10.4 (v0.181): "A static readonly field of class type may be marked
// `owned` … enrolling it in §22.13's program-exit disposal … Before v0.181
// the enum grammar had no `owned` slot … Appendix A.4 `StaticReadonlyDecl`
// now carries it." On HEAD the parser's modifier loop (parseMember) and
// checkEnumRestrictions were already shared with classes and never
// special-cased `owned`, so this half was already accepted; the tests below
// still lock in the codegen-level (disposal) behavior as the spec-derived
// conformance check #1564 asked for.
//
// The generic §22.13 exit-disposal machinery in
// src/codegen/LlvmBackend_Setup.cpp iterates ctx_.symbols() / TypeSymbol
// members without filtering by TypeDeclKind (class / static class / enum),
// so once sema stopped rejecting the declaration, the existing
// bz_register_static_owned / bz_dispose_static_owned atexit path (§22.13,
// #1212) applies unchanged — these tests exercise that reuse.
//
// Test plan:
//   1557-a  static class, single `static owned readonly` field with a
//           destructor → JIT: Main reads Res.n=0 before exit (field still
//           live); LSan: block freed at exit, no leak.
//   1557-b  static class, static owning-inline-array field `[N owned]T` →
//           same JIT/LSan shape (regression guard: this shape was already
//           accepted pre-fix — see sema/static_class_members_test.cpp — but
//           its disposal was never covered by a test).
//   1557-c  failed-init interaction (§22.7/§22.13): a static class's owned
//           field initializer succeeds (and is registered for atexit
//           disposal) BEFORE its static constructor throws. The type ends
//           up in the §22.7 permanently-failed state, but the already-
//           registered owned field must still be disposed at exit — the
//           registration happens at store time in emitStaticConstructorBody,
//           independent of whether the enclosing type's own init ultimately
//           succeeds. JIT: the wrapped TypeInitializationException is caught
//           and Res.n=0 (not yet disposed); LSan: block still freed, no leak.
//   1564-a  enum, single `static owned readonly` field with a destructor →
//           same JIT/LSan shape as 1557-a.
//   1564-b  enum, failed-init interaction mirroring 1557-c (a static
//           constructor throwing after an owned field initializer succeeds).

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

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "support/unique_stem.hpp"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compiles `source` with ASan and runs under LSan.  Returns:
//   0  — clean (no leak, no error)
//   1  — sanitizer flagged a problem (test failure)
//  -1  — toolchain unavailable / compile failed (SKIP, not a failure)
// Mirrors tests/codegen/static_owned_field_disposal_1212_test.cpp's helper.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_1557_1564_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    // ASan exit code 99 → sanitizer flagged a problem; otherwise clean.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                       // clean
}

// =====================================================================
// 1557-a: static class, single `static owned readonly` field
// =====================================================================

// Main is declared on Globals itself, so calling it triggers Globals' own
// lazy static init (§22.13 / #720) — the field never has to be touched in
// the body for its initializer to have run and been registered.
const char* kStaticClassSingleOwned =
    "module M; "
    "class Res "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.n := Res.n + 1; end destructor; "
    "end Res; "
    "static class Globals "
    "  public static owned readonly Cache: Res := new Res(); "
    "  public static method Main(): int => Res.n; "
    "end Globals;";

BEZEL_TEST(codegen_static_class_owned_1557, single_field_main_reads_zero_before_exit_11_7) {
    // §11.7/§22.13: Cache is still live during Main (disposal is deferred to
    // program exit), so Res.n reads 0. Before the fix this program did not
    // compile at all ("static class `Globals` may not declare an owned
    // field `Cache` (§11.7)").
    BEZEL_EXPECT_EQ(run(kStaticClassSingleOwned), 0);
}

BEZEL_TEST(codegen_static_class_owned_1557, single_field_block_freed_at_exit_no_leak_11_7_22_13) {
    // LSan: the atexit handler installed by bz_register_static_owned frees
    // the Res block at program exit — the same §22.13 mechanism #1212
    // verified for ordinary classes, now reachable from a static class.
    int lsan = runUnderLeakSanitizer(kStaticClassSingleOwned);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1557-b: static class, static owning-inline-array field `[N owned]T`
// =====================================================================

const char* kStaticClassOwnedInlineArray =
    "module M; "
    "class Slot "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Slot.n := Slot.n + 1; end destructor; "
    "end Slot; "
    "static class Globals "
    "  public static _arr: [2 owned]Slot? := [new Slot(), new Slot()]; "
    "  public static method Main(): int => Slot.n; "
    "end Globals;";

BEZEL_TEST(codegen_static_class_owned_1557, inline_array_main_reads_zero_before_exit_11_7) {
    // Both slots are live during Main; Slot.n is still 0.
    BEZEL_EXPECT_EQ(run(kStaticClassOwnedInlineArray), 0);
}

BEZEL_TEST(codegen_static_class_owned_1557, inline_array_all_slots_freed_no_leak_11_7_22_13) {
    // LSan: both Slot blocks are freed at exit, individually registered by
    // the same loop #1212 added to emitStaticConstructorBody.
    int lsan = runUnderLeakSanitizer(kStaticClassOwnedInlineArray);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1557-c: failed-init interaction (§22.7/§22.13)
// =====================================================================
//
// `Cache`'s field initializer runs (and is registered for atexit disposal)
// as part of Globals' synthesized sfinit, which always runs BEFORE the
// user-written static constructor (§22.13's "static fields initialize before
// the static constructor runs" ordering — see emitStaticConstructorBody).
// The static constructor then throws, so Globals' lazy init
// (bz$Globals$ensureinit) marks the type permanently failed (§22.7) and
// leaves a pending TypeInitializationException for the access site — here,
// reading `Globals.V` inside Main's try block — to route into the catch arm.

const char* kStaticClassFailedInitAfterOwnedField =
    "module M; "
    "class Res "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.n := Res.n + 1; end destructor; "
    "end Res; "
    "static class Globals "
    "  public static owned readonly Cache: Res := new Res(); "
    "  public static V: int := 0; "
    "  public static constructor() throw new Exception(\"boom\"); end constructor; "
    "end Globals; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      var v := Globals.V; "
    "      return -1; "
    "    catch TypeInitializationException => return Res.n; "
    "    end; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_static_class_owned_1557, failed_init_catches_wrapped_exception_res_still_live_22_7) {
    // Res.n=0: Cache's initializer completed (the field IS live) but its
    // destructor has not run yet — disposal is deferred to program exit
    // regardless of the static ctor's later failure. Returning -1 would mean
    // the failure did not surface (regression); any other value would mean
    // Res.n was already incremented before Main observed it.
    BEZEL_EXPECT_EQ(run(kStaticClassFailedInitAfterOwnedField), 0);
}

BEZEL_TEST(codegen_static_class_owned_1557, failed_init_owned_field_still_freed_no_leak_22_7_22_13) {
    // LSan: even though Globals' own static init ends in the §22.7 failed
    // state, Cache was already registered via bz_register_static_owned at
    // store time (before the throwing static ctor ran), so the atexit
    // handler still frees its block. No leak.
    int lsan = runUnderLeakSanitizer(kStaticClassFailedInitAfterOwnedField);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1564-a: enum, single `static owned readonly` field
// =====================================================================

const char* kEnumSingleOwned =
    "module M; "
    "class Res "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.n := Res.n + 1; end destructor; "
    "end Res; "
    "enum Kind "
    "  A, B; "
    "  public static owned readonly Cache: Res := new Res(); "
    "  public static method Main(): int => Res.n; "
    "end Kind;";

BEZEL_TEST(codegen_enum_owned_1564, single_field_main_reads_zero_before_exit_10_4) {
    // §10.4/§22.13: Cache is still live during Main. Main is declared
    // directly on the enum (§33.2/§33.5 place no restriction on the
    // declaring type kind), so calling it triggers Kind's own lazy init.
    BEZEL_EXPECT_EQ(run(kEnumSingleOwned), 0);
}

BEZEL_TEST(codegen_enum_owned_1564, single_field_block_freed_at_exit_no_leak_10_4_22_13) {
    // LSan: the Res block registered by Kind's sfinit is freed at exit —
    // the same §22.13/#1212 mechanism, now reachable from an enum body.
    int lsan = runUnderLeakSanitizer(kEnumSingleOwned);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1564-b: enum failed-init interaction, mirroring 1557-c
// =====================================================================

// The throwing leg is a LATER static readonly field initializer, not a static
// constructor: §10.4's admitted-static list for an enum names `const` members,
// static methods and `static readonly` fields only, and states that "a mutable
// static field, a `static` property, and a static constructor are not among
// them". Fix #3840 (§10.4 / §35.2, v0.288) enforces exactly that, and
// tests/lit/sema/sema_enum__static_constructor_rejected_3840.bzl pins the
// rejection. The earlier spelling of this fixture used `public static V: int`
// plus a `public static constructor()` that threw — a program §10.5 forbids
// outright, so it never reached codegen: both rows failed on a sema rejection
// (`run` → -999, the ASan helper → asanBuildFailure("sema") = -2) and asserted
// nothing about disposal. `Boom.Fire()` reproduces the intended shape on a
// legal enum: §22.7 puts an uncaught throw from "the static field initializers,
// the static constructor body, or both" into the same permanently-failed state,
// so a throwing field initializer is the enum-legitimate spelling of the same
// event.
//
// §22.13 orders the two initializers textually, which is what makes Cache's
// registration happen before V's initializer throws — the same precondition the
// static-class 1557-c fixture gets from "field initializers run before the
// static constructor".
const char* kEnumFailedInitAfterOwnedField =
    "module M; "
    "class Res "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.n := Res.n + 1; end destructor; "
    "end Res; "
    "class Boom "
    "  public static method Fire(): int => throw new Exception(\"boom\"); "
    "end Boom; "
    "enum Kind "
    "  A, B; "
    "  public static owned readonly Cache: Res := new Res(); "
    "  public static readonly V: int := Boom.Fire(); "
    "end Kind; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      var v := Kind.V; "
    "      return -1; "
    "    catch TypeInitializationException => return Res.n; "
    "    end; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_enum_owned_1564, failed_init_catches_wrapped_exception_res_still_live_22_7) {
    // Same reasoning as the static-class 1557-c case: Cache's initializer
    // completed and was registered before a later initializer threw, so the
    // §22.7 failure surfaces as TypeInitializationException at the `Kind.V`
    // access (returning -1) while Cache is still undisposed (Res.n = 0).
    BEZEL_EXPECT_EQ(run(kEnumFailedInitAfterOwnedField), 0);
}

BEZEL_TEST(codegen_enum_owned_1564, failed_init_owned_field_still_freed_no_leak_22_7_22_13) {
    // LSan: Cache's block is still freed at exit despite Kind's own static
    // init ending in the §22.7 failed state. Note the `== -1` guard is
    // toolchain-absence only; a build failure returns -2
    // (asanBuildFailure, #4563) and therefore fails this row loudly rather
    // than passing vacuously — which is how the illegal spelling surfaced.
    int lsan = runUnderLeakSanitizer(kEnumFailedInitAfterOwnedField);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// #5472: runtime-level slot-nulling test
// Direct test of Phase A of bz_dispose_static_owned's two-phase drain.
// Verifies that the static field slot is nulled after disposal, making
// the allocation unreachable so LSan can detect a leak if disposal is skipped.
// =====================================================================
// The runtime exports bz_register_static_owned and bz_flush_static_owned
// (linked via -Wl,--whole-archive, confirmed by nm -C on the test binary).
// Six existing tests already call bz_flush_static_owned directly.
extern "C" void bz_register_static_owned(void* obj, void** slotAddr);
extern "C" void bz_flush_static_owned(void);

// Fake vtable: {nullptr, nullptr, dispose_fn}
// dispose_fn bumps a counter so we can verify disposal ran.
static int g_disposeCount = 0;
static void fake_dispose(void*) { ++g_disposeCount; }
static void* g_slot = nullptr;  // Separate slot global, matching codegen's layout

BEZEL_TEST(codegen_static_owned_disposal_1212, slot_nulled_after_disposal_phase_a_5472) {
    // Arrange: fake vtable and separate slot global
    void* vtable[3] = {nullptr, nullptr, (void*)&fake_dispose};
    void* obj = malloc(16);  // payload + padding
    *(void**)obj = vtable;   // vtable at offset 0
    g_slot = obj;

    // Act: register and flush (simulates atexit disposal)
    bz_register_static_owned(obj, &g_slot);
    g_disposeCount = 0;
    bz_flush_static_owned();

    // Assert: slot is nulled (Phase A) and disposal ran (Phase B)
    BEZEL_EXPECT_EQ(g_slot, nullptr);
    BEZEL_EXPECT_EQ(g_disposeCount, 1);
}

} // namespace
