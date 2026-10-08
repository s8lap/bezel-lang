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
// These tests use the KEYWORD-LESS form (`: ISerializable`/`: IDeserializable`,
// no `serialized` clause) — the v0.141 type-driven form. A sema pre-pass
// synthesizes the wire shape from field types (graph when a Shared<T>/Weak<T>/
// borrow component is present, tree otherwise), so these classes reach the
// Shared/Weak identity codegen without any keyword.
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
#include "support/emit_object_support.hpp"
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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

using bezel::test::parseAndCheckOwnership;
using bezel::test::runJit;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir703() {
    return wtScratchDir("wt-703");
}

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
    auto dir = wtDir703();
    std::string stem = unique_stem("bezel_703_lsan");
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
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
    "class Inner : ISerializable, IDeserializable "
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
// Keyword-less entry: the pure v0.141 type-driven form (NO keyword clause)
// ===========================================================================

// §9.8 v0.141: the headline of the migration — a class declares `: ISerializable,
// IDeserializable` with NO `serialized graph` keyword, and the Shared<Inner>
// field drives identity serialization purely from its type. Round-trips (42),
// LSan-clean. This is the form the v0.141 spec describes; the keyword is gone.
BEZEL_TEST(serialization_typedriven_703,
           keyword_less_shared_field_roundtrip_spec_9_8_v0141) {
    expectLeakFree(
        std::string(kImport) +
        "class Inner: ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Inner; "
        "class Holder: ISerializable, IDeserializable "
        "  public owned Handle: Shared<Inner>; "
        "  public constructor(owned handle: Shared<Inner>) "
        "    Handle := handle; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var inner := new Inner(42); "
        "  using var sh := new Shared<Inner>(move inner); "
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

// §9.8 v0.141: keyword-less self-cycle — a Node declaring `: ISerializable,
// IDeserializable` (no keyword) with a Weak<Node> self-back-edge round-trips and
// the weak upgrades back to the node. Proves the cyclic capability works in the
// pure type-driven form.
BEZEL_TEST(serialization_typedriven_703,
           keyword_less_cyclic_self_weak_roundtrip_spec_9_8_v0141) {
    expectLeakFree(
        std::string(kImport) +
        "class Node: ISerializable, IDeserializable "
        "  public property V: int; "
        "  public owned Back: Weak<Node>?; "
        "  public constructor(v: int, owned back: Weak<Node>?) "
        "    V := v; Back := back; "
        "  end constructor; "
        "end Node; "
        "class Holder: ISerializable, IDeserializable "
        "  public owned Root: Shared<Node>; "
        "  public constructor(owned root: Shared<Node>) "
        "    Root := root; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var node := new Node(42, null); "
        "  using var sh := new Shared<Node>(move node); "
        "  var n := sh.Get(); n.Back := sh.Downgrade(); "
        "  using var holder := new Holder(sh.Clone()); "
        "  using var s := new BinarySerializer(); "
        "  holder.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var holder2 := Holder.Deserialize(d); "
        "  if holder2.Root.Get().Back!.TryUpgrade(out using var up) then "
        "    return holder2.Root.Get().V + up.Get().V; "
        "  end; "
        "  return -1; "
        "end Main; end C;",
        84);
}

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
        "class Holder : ISerializable, IDeserializable "
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
        "class Holder : ISerializable, IDeserializable "
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
        "class Node : ISerializable, IDeserializable "
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
        "class Node : ISerializable, IDeserializable "
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
        "class Node : ISerializable, IDeserializable "
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
        "class Holder : ISerializable, IDeserializable "
        "  public owned Handle: Shared<Inner>; "
        "  public constructor(owned handle: Shared<Inner>) "
        "    Handle := handle; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var inner := new Inner(42); "
        "  using var sh := new Shared<Inner>(move inner); "
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
        "class Pair : ISerializable, IDeserializable "
        "  public owned First: Shared<Inner>; "
        "  public owned Second: Shared<Inner>; "
        "  public constructor(owned first: Shared<Inner>, "
        "                     owned second: Shared<Inner>) "
        "    First := first; Second := second; "
        "  end constructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var inner := new Inner(42); "
        "  using var sh := new Shared<Inner>(move inner); "
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

// ===========================================================================
// S3: owned Weak<class> back-reference round-trip (DAG, §9.8 v0.141, #703)
// ===========================================================================

// §9.8 v0.141: a class with an owned Shared<Inner> participant AND an owned
// Weak<Inner> back-reference to the SAME value. On serialize the Shared writes
// inline+$id and the Weak writes $ref to that tag (keyed on bz_shared_get). On
// deserialize the Shared constructs+wraps+registers the handle and the Weak
// resolves the tag and bz_shared_downgrade's it — so the deserialized Weak
// upgrades back to the same live Inner. Returns Strong.V + upgraded-weak.V (84),
// proving the weak resolved to the right object; disposal is leak-free (the weak
// release + strong release balance to one disposal of the deserialized Inner).
BEZEL_TEST(serialization_typedriven_703,
           s3_owned_weak_backref_dag_roundtrip_spec_9_8_v0141) {
    expectLeakFree(
        std::string(kImport) + kEligibleInner +
        "class Node : ISerializable, IDeserializable "
        "  public owned Strong: Shared<Inner>; "
        "  public owned Weakref: Weak<Inner>; "
        "  public constructor(owned strong: Shared<Inner>, "
        "                     owned weakref: Weak<Inner>) "
        "    Strong := strong; Weakref := weakref; "
        "  end constructor; "
        "end Node; "
        "class C public static method Main(): int "
        "  var inner := new Inner(42); "
        "  using var sh := new Shared<Inner>(move inner); "
        "  using var n := new Node(sh.Clone(), sh.Downgrade()); "
        "  using var s := new BinarySerializer(); "
        "  n.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var n2 := Node.Deserialize(d); "
        "  if n2.Weakref.TryUpgrade(out using var up) then "
        "    return n2.Strong.Get().V + up.Get().V; "
        "  end; "
        "  return -1; "
        "end Main; end C;",
        84);
}

// ===========================================================================
// S4: nullable Shared<class>? participant (§9.8 v0.141, #703)
// ===========================================================================

// §9.8 v0.141: a nullable owned Shared<Inner>? field holding a value round-trips
// like the non-nullable case (the null marker path is not taken). Exit = 42.
BEZEL_TEST(serialization_typedriven_703,
           s4_nullable_shared_present_roundtrip_spec_9_8_v0141) {
    expectLeakFree(
        std::string(kImport) + kEligibleInner +
        "class Box : ISerializable, IDeserializable "
        "  public owned Item: Shared<Inner>?; "
        "  public constructor(owned item: Shared<Inner>?) "
        "    Item := item; "
        "  end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  var inner := new Inner(42); "
        "  using var sh := new Shared<Inner>(move inner); "
        "  using var b := new Box(sh.Clone()); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Box.Deserialize(d); "
        "  if b2.Item <> null then return b2.Item!.Get().V; end; "
        "  return -1; "
        "end Main; end C;",
        42);
}

// §9.8 v0.141: a nullable owned Shared<Inner>? field that is NULL writes/reads
// the null marker and round-trips as null. Returns 7 via the null branch.
BEZEL_TEST(serialization_typedriven_703,
           s4_nullable_shared_null_roundtrip_spec_9_8_v0141) {
    expectLeakFree(
        std::string(kImport) + kEligibleInner +
        "class Box : ISerializable, IDeserializable "
        "  public owned Item: Shared<Inner>?; "
        "  public constructor(owned item: Shared<Inner>?) "
        "    Item := item; "
        "  end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(null); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := Box.Deserialize(d); "
        "  if b2.Item = null then return 7; end; "
        "  return -1; "
        "end Main; end C;",
        7);
}

// §28.13 / #703 prerequisite: a Shared/Weak cycle disposed via its SOLE strong
// owner must not double-free its control block. A owns Shared<B>, B holds a
// Weak<A> back-edge; releasing A's last strong handle runs A's destructor, which
// disposes B, whose destructor releases the Weak<A> back-edge — a re-entrant
// bz_weak_release on A's own control block while bz_shared_release(A) is still on
// the stack. Before the temp-weak-hold fix this double-freed the block. This is
// the runtime prerequisite for cyclic serialization (the deserialized graph has
// exactly this sole-ownership shape).
BEZEL_TEST(serialization_typedriven_703,
           shared_weak_cycle_sole_owner_disposal_no_double_free_28_13) {
    std::string src =
        "module M; "
        "class Node "
        "  public V: int := 0; "
        "  public owned Peer: Shared<Node>?; "
        "  public owned Back: Weak<Node>?; "
        "  public constructor(v: int, owned peer: Shared<Node>?, "
        "                     owned back: Weak<Node>?) "
        "    V := v; Peer := peer; Back := back; "
        "  end constructor; "
        "end Node; "
        "class C public static method Main(): int "
        "  var nodeB := new Node(2, null, null); "
        "  var nodeA := new Node(1, new Shared<Node>(move nodeB), null); "  // §22.3 v0.169
        "  using var shA := new Shared<Node>(move nodeA); "  // §22.3 v0.169

        "  var pn := shA.Get().Peer!.Get(); pn.Back := shA.Downgrade(); "
        "  return shA.Get().V; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
    int lsan = runUnderLeakSanitizer(src);
    BEZEL_EXPECT_EQ(lsan, 0);
}

// ===========================================================================
// S5: CYCLIC round-trip — Shared spine + Weak self-back-edge (§9.8 v0.141, #703)
// ===========================================================================

// §9.8 v0.141 — the headline cyclic capability. A Node is a Shared participant
// (reached via Holder.Root) and holds a Weak<Node> back-edge to ITSELF. On
// serialize the participant's tag is assigned before its body, so the weak-back
// inside the body writes a $ref to that tag. On deserialize the placeholder
// handle is registered under the tag BEFORE the body is constructed, so the
// weak-back resolves it (in-progress) and downgrades it; the value is filled
// after construction (bz_shared_set_value). The reconstructed node's weak-back
// therefore upgrades to the node itself (42 + 42 = 84). Disposal of the
// sole-owned cycle is leak-free thanks to the bz_shared_release temp-weak fix.
BEZEL_TEST(serialization_typedriven_703,
           s5_cyclic_self_weak_back_edge_roundtrip_spec_9_8_v0141) {
    expectLeakFree(
        std::string(kImport) +
        "class Node : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public owned Back: Weak<Node>?; "
        "  public constructor(v: int, owned back: Weak<Node>?) "
        "    V := v; Back := back; "
        "  end constructor; "
        "end Node; "
        "class Holder : ISerializable, IDeserializable "
        "  public owned Root: Shared<Node>; "
        "  public constructor(owned root: Shared<Node>) "
        "    Root := root; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var node := new Node(42, null); "
        "  using var sh := new Shared<Node>(move node); "
        "  var n := sh.Get(); n.Back := sh.Downgrade(); "
        "  using var holder := new Holder(sh.Clone()); "
        "  using var s := new BinarySerializer(); "
        "  holder.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var holder2 := Holder.Deserialize(d); "
        "  if holder2.Root.Get().Back!.TryUpgrade(out using var up) then "
        "    return holder2.Root.Get().V + up.Get().V; "
        "  end; "
        "  return -1; "
        "end Main; end C;",
        84);
}

// §9.8 v0.141 / #703: a RECURSIVE Shared type — Node has a Shared<Node> field —
// is not yet supported (inline serialize emission would recurse on the element's
// own Serialize at codegen time; a synthesized Serialize FUNCTION is required).
// Until that lands, such a class must produce a clean codegen error rather than
// crashing the compiler with unbounded emitClassSerialize recursion.
BEZEL_TEST(serialization_typedriven_703,
           s5_recursive_shared_type_codegen_guarded_703) {
    BEZEL_EXPECT_TRUE(guardedAtCodegen(
        std::string(kImport) +
        "class Node : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public owned Next: Shared<Node>?; "
        "  public constructor(v: int, owned next: Shared<Node>?) "
        "    V := v; Next := next; "
        "  end constructor; "
        "end Node; "
        "class Holder : ISerializable, IDeserializable "
        "  public owned Root: Shared<Node>; "
        "  public constructor(owned root: Shared<Node>) "
        "    Root := root; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var node := new Node(1, null); "
        "  using var sh := new Shared<Node>(move node); "
        "  using var holder := new Holder(sh.Clone()); "
        "  using var s := new BinarySerializer(); "
        "  holder.Serialize(s); "
        "  return 0; "
        "end Main; end C;"));
}
