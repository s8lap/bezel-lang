// Spec-derived sema tests — §9.8 type-driven serialization (v0.141), issue #703.
//
// SLICE S1 (sema eligibility): serialization-eligibility admits Shared<T> and
// Weak<T> component types, recursing on the element. Under the type-driven
// model (spec v0.141) a class participates in serialization by declaring
// `: ISerializable`/`: IDeserializable` and the field TYPES drive the wire
// shape: an owned/value field is an owning (inline) edge; a `Shared<T>` field
// is an identity participant ($id/$ref); a `Weak<T>` (or borrow) field is a
// back-reference ($ref). The eligibility rule (§9.8) is the structural gate: a
// component of type `Shared<T>`/`Weak<T>` is serialization-eligible iff T is.
//
// Migration sequencing: slices S1–S8 are ADDITIVE — the keyword path
// (`serialized tree`/`graph`/`cyclic`) stays active (the parser still accepts
// it) until S9 removes it. These tests therefore exercise the eligibility
// FUNCTION through the still-active keyword path (a `serialized graph` class
// with a `Shared<T>`/`Weak<T>` field), which is the smallest vehicle that
// reaches `isSerializationEligibleType` for those component types.
//
// SCOPE ISOLATION: a `serialized` clause synthesizes BOTH directions, so the
// IDeserializable constructor-selection rule (§9.8 v0.136 — every component must
// be a name-matched constructor parameter) always applies. The positive cases
// here satisfy that rule (each component is a matching ctor param) so that the
// ONLY diagnostic the eligibility change can flip is the eligibility one. The
// Weak<T> field is passed as a constructor parameter purely to satisfy the rule
// in isolation; the type-driven back-edge wiring (Downgrade post-construction)
// is a later slice's concern, not eligibility's.
//
// Tested at the SEMA layer only (parseAndCheckOwnership) — no codegen/JIT —
// because the type-driven SYNTHESIS (Shared identity tagging, Weak back-refs)
// lands in later slices (S2+). Asserting at the sema layer isolates the
// eligibility change and avoids triggering not-yet-implemented synthesis.
//
// All APIs verified against Bezel_Standard_Library.md. No invented APIs.
// Spec refs: §9.8 (v0.141), §28.13, stdlib §2.14/§2.16.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
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
#include "support/Diagnostic.hpp"
#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

using bezel::test::parseAndCheckOwnership;
using bezel::test::runJit;

// Run a program through the JIT; return its exit code, or -999 if it did not run.
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// Compile to a native object, link with ASan/LSan, run. Returns 0 (clean),
// 1 (sanitizer triggered), or -1 (toolchain unavailable → skip). Mirrors the
// shared_weak_nullable_620 / serialization_tier2 pattern exactly.
int runUnderLeakSanitizer(const std::string& source) {
    namespace fs = std::filesystem;
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_703_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    {
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
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                       // clean
}

// Assert a program returns `expectedExit` AND is LeakSanitizer-clean.
void expectLeakFree(const std::string& src, int expectedExit) {
    BEZEL_EXPECT_EQ(run(src), expectedExit);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan == -1) return;  // toolchain unavailable → skip the LSan assertion
    BEZEL_EXPECT_EQ(lsan, 0);
}

// True iff the program is accepted (no errors) through ownership.
bool semaAccepts(std::string_view src) {
    auto r = parseAndCheckOwnership(src);
    return !r.diagnostics.hasErrors();
}

// True iff the program is rejected with the §9.8 not-serialization-eligible
// diagnostic (rather than some unrelated error).
bool rejectsAsIneligible(std::string_view src) {
    auto r = parseAndCheckOwnership(src);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not serialization-eligible") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff compiling `src` fails with the #703 codegen GUARD diagnostic — the
// clean "Shared/Weak wire codegen not yet implemented" error that prevents a
// silent miscompile of a Shared/Weak component once S1 admits it at sema.
bool guardedAtCodegen(std::string_view src) {
    auto r = runJit(std::string(src));
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not yet implemented in codegen") !=
                std::string::npos &&
            d.message.find("Shared<T>") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// An eligible leaf class: a `serialized tree` class with a single int field.
const char* kEligibleInner =
    "class Inner serialized tree : ISerializable, IDeserializable "
    "  public property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Inner; ";

// An INELIGIBLE leaf class: no graph-shape clause, no ISerializable.
const char* kPlainInner =
    "class Plain "
    "  public property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Plain; ";

const char* kImport = "module M; import System.Serialization; ";

} // namespace

// ===========================================================================
// S1: Shared<T> component eligibility (§9.8 v0.141)
// ===========================================================================

// §9.8 v0.141: a class with an owned `Shared<Inner>` field, where Inner is
// serialization-eligible, is itself serialization-eligible. (Before S1 the
// `Shared<Inner>` field was rejected as ineligible because Shared<T> was not
// in the eligible component set.)
BEZEL_TEST(serialization_typedriven_703,
           s1_shared_field_of_eligible_class_accepted_spec_9_8_v0141) {
    BEZEL_EXPECT_TRUE(semaAccepts(
        std::string(kImport) + kEligibleInner +
        "class Holder serialized graph : ISerializable, IDeserializable "
        "  public owned Handle: Shared<Inner>; "
        "  public constructor(owned handle: Shared<Inner>) "
        "    Handle := handle; "
        "  end constructor; "
        "end Holder; "));
}

// §9.8 v0.141: `Shared<Plain>` where Plain is NOT serialization-eligible →
// the recursion on the element rejects it. The diagnostic must name field-level
// ineligibility, not silently accept.
BEZEL_TEST(serialization_typedriven_703,
           s1_shared_field_of_ineligible_class_rejected_spec_9_8_v0141) {
    BEZEL_EXPECT_TRUE(rejectsAsIneligible(
        std::string(kImport) + kPlainInner +
        "class Holder serialized graph : ISerializable, IDeserializable "
        "  public owned Handle: Shared<Plain>; "
        "  public constructor(owned handle: Shared<Plain>) "
        "    Handle := handle; "
        "  end constructor; "
        "end Holder; "));
}

// ===========================================================================
// S1: Weak<T> component eligibility (§9.8 v0.141)
// ===========================================================================

// §9.8 v0.141: a nullable `Weak<Inner>?` back-reference field, where Inner is
// serialization-eligible, is itself eligible (serialized as a $ref). Weak<T>
// is non-owning, so the field carries no disposal obligation.
BEZEL_TEST(serialization_typedriven_703,
           s1_weak_field_of_eligible_class_accepted_spec_9_8_v0141) {
    BEZEL_EXPECT_TRUE(semaAccepts(
        std::string(kImport) + kEligibleInner +
        "class Node serialized graph : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public Back: Weak<Inner>?; "
        "  public constructor(v: int, back: Weak<Inner>?) "
        "    V := v; "
        "    Back := back; "
        "  end constructor; "
        "end Node; "));
}

// §9.8 v0.141: `Weak<Plain>?` where Plain is NOT serialization-eligible →
// rejected by the recursion on the element.
BEZEL_TEST(serialization_typedriven_703,
           s1_weak_field_of_ineligible_class_rejected_spec_9_8_v0141) {
    BEZEL_EXPECT_TRUE(rejectsAsIneligible(
        std::string(kImport) + kPlainInner +
        "class Node serialized graph : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public Back: Weak<Plain>?; "
        "  public constructor(v: int, back: Weak<Plain>?) "
        "    V := v; "
        "    Back := back; "
        "  end constructor; "
        "end Node; "));
}

// ===========================================================================
// S2: codegen guard — still-unhandled cases (§9.8 v0.141, #703)
// ===========================================================================

// S2 implements Shared<class> identity codegen, but Weak<T> wire codegen is not
// yet emitted. Calling Serialize on a class with a Weak<T> component must still
// produce a clean "not yet implemented" codegen error — NOT a silent miscompile.
// (When the Weak slice lands, this flips to a round-trip test.)
BEZEL_TEST(serialization_typedriven_703,
           s2_weak_component_serialize_codegen_guarded_703) {
    BEZEL_EXPECT_TRUE(guardedAtCodegen(
        std::string(kImport) + kEligibleInner +
        "class Node serialized graph : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public Back: Weak<Inner>?; "
        "  public constructor(v: int, back: Weak<Inner>?) "
        "    V := v; "
        "    Back := back; "
        "  end constructor; "
        "end Node; "
        "class C public static method Main(): int "
        "  using var n := new Node(1, null); "
        "  using var s := new BinarySerializer(); "
        "  n.Serialize(s); "
        "  return 0; "
        "end Main; end C;"));
}

// ===========================================================================
// S2: Shared<class> identity round-trip (§9.8 v0.141, #703)
// ===========================================================================

// §9.8 v0.141: round-trip a class with a single owned Shared<Inner> field. The
// Shared participant is written inline with an identity tag on serialize, and on
// deserialize the element is constructed, wrapped in a fresh Shared control
// block, and stored — so the value survives the round-trip and disposal is clean
// (the deserialized handle releases its one strong reference → Inner disposed
// exactly once). Exit code = the round-tripped Inner.V.
BEZEL_TEST(serialization_typedriven_703,
           s2_shared_field_roundtrip_value_preserved_spec_9_8_v0141) {
    expectLeakFree(
        std::string(kImport) + kEligibleInner +
        "class Holder serialized graph : ISerializable, IDeserializable "
        "  public owned Handle: Shared<Inner>; "
        "  public constructor(owned handle: Shared<Inner>) "
        "    Handle := handle; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var inner := new Inner(42); "
        "  using var sh := new Shared<Inner>(inner); "
        "  using var h := new Holder(sh.Clone()); "
        "  using var s := new BinarySerializer(); "
        "  h.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var h2 := Holder.Deserialize(d); "
        "  return h2.Handle.Get().V; "
        "end Main; end C;",
        42);
}

// §9.8 v0.141: two owned Shared<Inner> fields that ALIAS the same Inner. On
// serialize the first writes inline+$id and the second writes $ref to the same
// tag (keyed on bz_shared_get, the underlying value). On deserialize the first
// constructs+wraps+registers the handle and the second resolves the tag and
// bz_shared_clone's it — so BOTH fields share one control block (one Inner,
// strong=2). Both .Get().V read 42 (→ 84), and disposal is leak-free: the two
// handles release to one disposal of the single deserialized Inner.
BEZEL_TEST(serialization_typedriven_703,
           s2_shared_alias_identity_preserved_spec_9_8_v0141) {
    expectLeakFree(
        std::string(kImport) + kEligibleInner +
        "class Pair serialized graph : ISerializable, IDeserializable "
        "  public owned First: Shared<Inner>; "
        "  public owned Second: Shared<Inner>; "
        "  public constructor(owned first: Shared<Inner>, "
        "                     owned second: Shared<Inner>) "
        "    First := first; Second := second; "
        "  end constructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var inner := new Inner(42); "
        "  using var sh := new Shared<Inner>(inner); "
        "  using var p := new Pair(sh.Clone(), sh.Clone()); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var p2 := Pair.Deserialize(d); "
        "  return p2.First.Get().V + p2.Second.Get().V; "
        "end Main; end C;",
        84);
}
