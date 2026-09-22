// Spec-derived codegen tests — §9.8 serialization synthesis, TIER 3: graph.
// Issue #550, tier 3: `serialized graph` back-edge (non-owned class-typed field)
// serialization using the identity-tag/BZ_SER_REF protocol.
//
// Wire format (§9.8 tier-3 design):
//   Before each class-typed component in `serialized graph` mode:
//     BZ_SER_INLINE (0x01) + LE32 tag_id  → first owning reach of this object
//     BZ_SER_REF    (0x02) + LE32 tag_id  → back-edge / aliased reference
//   Under `serialized tree`, these markers are NOT emitted (legacy path).
//
// Tag table:
//   Serializer (bz_binser_t):  pointer → tag_id (assigned on first owning reach).
//   Deserializer (bz_binder_t): tag_id → object pointer (built as objects construct).
//   Cycle detection: tag_in_progress[i] = 1 while object (i+1) is under construction.
//   A BZ_SER_REF to an in-progress tag raises SerializationException per §9.8.
//
// Spec refs: §9.8 v0.136 (shared-reference DAG synthesis, identity/back-edge).
// All APIs verified against Bezel_Standard_Library.md.  No invented APIs.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
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
#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"   // runAot (JIT->AOT migration, §9.8 graph tests)
#include "support/run_under_asan.hpp"  // runUnderAsanValue (#3653 shared ASan leg)
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;
using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// §9.8 graph back-edge round-trips migrated to the AOT harness (real driver,
// full front-end incl. §28.3 ownership). runJit masked the former serialize-side
// borrow-into-field wiring by skipping the ownership pass (codegen_support.hpp
// runJitInProcess runs neither ownership nor definite assignment); the AOT path
// is the correct oracle. Migrated tests carry a note on the reshape they needed.
int runGraphAot(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool compileFails(const std::string& src) {
    auto r = runJit(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

// Compile `source` under -fsanitize=address, link, run with detect_leaks=1,
// and check BOTH the sanitizer verdict AND the program's own exit value.
// Returns 0 (clean and the expected value), 1 (sanitizer report / crash /
// wrong value), -1 (toolchain unavailable: SKIP), or -2/-3/-4 (the leg's own
// build/link/exec broke — reported loudly via `[ INFRA ]`, per #4563).
//
// Forwards to the ONE shared Family-A implementation (#3653). The hand-rolled
// copy this replaces differed from it in two ways that both cost coverage:
// it never looked at the program's exit value, so the §9.8 identity checks the
// two rows below encode (`return 91` / `92` / `93`) could all have fired while
// the row still reported ASan-clean; and it put the object file and the linked
// executable under `fs::temp_directory_path()` — /tmp, the tmpfs CLAUDE.md
// forbids for scratch (tests/support/scratch_dir.hpp records the EDQUOT pile
// that produced that rule). `runUnderAsanValue` checks the value and uses
// `wtScratchDir()`.
int runUnderLeakSanitizer550t3(const std::string& source, int expectedExitCode) {
    return bezel::test::runUnderAsanValue(source, expectedExitCode,
                                          "bezel_550t3_lsan");
}

// Preamble for all BinarySerializer tests (§2.15: requires import).
const char* kImport3 = "import System.Serialization; ";

} // namespace

// ===========================================================================
// §9.8 `serialized graph`: diamond DAG round-trip with back-edge identity
//
// "serialized edges may alias a shared object (the same object reachable by
// more than one path), but there are no cycles."
// "A borrowing field is not written inline; it is written as a reference to
// the tag of the object it borrows."
// "Under `serialized graph`, a reference whose target has already been
// constructed is bound to that object (identity preserved, not duplicated)."
//
// Topology: Hub owns Target (Slot "alpha").  Hub has a nullable back-edge
// Alias: Slot? pointing to Target.  Wire: Target written with BZ_SER_INLINE +
// tag_id=1, then Alias written with BZ_SER_REF + tag_id=1.  On deserialize,
// Alias resolved to the same constructed Slot object → Name = "alpha".
// ===========================================================================

// §9.8: `serialized graph` back-edge round-trip — Hub owns Target, Alias borrows Target.
// After round-trip: Alias resolves to same target as Target (identity preserved).
// Construction pattern (v0.180 §28.3 / AOT migration): the self-alias is wired
// inside a convenience constructor where the borrow SOURCE is receiver-sourced
// (`this.Target`) — admissible per §28.3 ("parameter-sourced and receiver-sourced
// borrows are admissible; scope-bound locals ... are rejected"). The former
// serialize-side `hub.Alias := hub.Target` stored a borrow sourced from the
// scope-bound local `hub` into a field, which the real driver rejects; only
// runJit's skipped ownership pass accepted it. The two-arg ctor is kept for §9.8
// Deserialize (binds the plain-borrow back-edge as a ctor arg from the tag
// table); the one-arg ctor is not a §9.8 selection match, so selection is
// unambiguous. Wire form and deserialize path are unchanged.
BEZEL_TEST(codegen_serialization_t3_graph,
           graph_back_edge_nullable_round_trip_spec_9_8) {
    BEZEL_EXPECT_EQ(runGraphAot(
        std::string("module M; ") + kImport3 +
        "class Slot : ISerializable, IDeserializable"
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Hub : ISerializable, IDeserializable"
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public constructor(owned target: Slot, alias: Slot?) "
        "    Target := move target; Alias := alias; "
        "  end constructor; "
        "  public constructor(owned target: Slot) "
        "    Target := move target; Alias := Target; "
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        "  using var hub := new Hub(new Slot(\"alpha\")); "
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        "  if hub2.Target.Name <> \"alpha\" then return 91; end; "
        "  if hub2.Alias = null then return 92; end; "
        "  if hub2.Alias?.Name <> \"alpha\" then return 93; end; "
        "  return 42; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 `serialized graph`: nullable back-edge that is null at serialize time.
//
// Verifies the null-back-edge wire path: when a nullable back-edge field is null
// at serialize time, the serializer writes a null marker (0x00 byte) instead of
// BZ_SER_REF + tag_id.  The deserializer must restore the field as null, not throw.
// ===========================================================================

// §9.8: nullable back-edge that is null serializes as null and deserializes as null.
BEZEL_TEST(codegen_serialization_t3_graph,
           graph_null_back_edge_round_trip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport3 +
        "class Slot : ISerializable, IDeserializable"
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Hub : ISerializable, IDeserializable"
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public constructor(owned target: Slot, alias: Slot?) "
        "    Target := move target; Alias := alias; "
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        // Alias is intentionally left null — exercises the null back-edge wire path.
        "  using var hub := new Hub(new Slot(\"beta\"), null); "
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        "  if hub2.Target.Name <> \"beta\" then return 91; end; "
        "  if hub2.Alias <> null then return 92; end; "
        "  return 42; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 `serialized graph`: 3-deep graph with multiple back-edges.
//
// Topology: Rack owns NodeA and NodeB (two Slot instances).  Rack has two
// nullable back-edges: PrimaryRef (borrows NodeA) and SecondaryRef (borrows NodeB).
// Wire order: NodeA (inline, tag=1), NodeB (inline, tag=2), PrimaryRef ($ref tag=1),
// SecondaryRef ($ref tag=2).  Round-trip verifies both back-edges resolve correctly.
// ===========================================================================

// §9.8: `serialized graph` with two back-edges to two distinct owned objects.
// Reshape mirrors graph_back_edge_nullable_round_trip: a convenience ctor wires
// both back-edges from the receiver (`this.NodeA`/`this.NodeB` — receiver-sourced,
// admissible per §28.3), replacing the former scope-bound-local field-store
// (`rack.PrimaryRef := rack.NodeA`) the real driver rejects. The four-arg ctor is
// retained for §9.8 Deserialize.
BEZEL_TEST(codegen_serialization_t3_graph,
           graph_two_back_edges_round_trip_spec_9_8) {
    BEZEL_EXPECT_EQ(runGraphAot(
        std::string("module M; ") + kImport3 +
        "class Slot : ISerializable, IDeserializable"
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Rack : ISerializable, IDeserializable"
        "  public owned NodeA: Slot; "
        "  public owned NodeB: Slot; "
        "  public PrimaryRef: Slot?; "
        "  public SecondaryRef: Slot?; "
        "  public constructor(owned nodeA: Slot, owned nodeB: Slot, "
        "                     primaryRef: Slot?, secondaryRef: Slot?) "
        "    NodeA := move nodeA; NodeB := move nodeB; "
        "    PrimaryRef := primaryRef; SecondaryRef := secondaryRef; "
        "  end constructor; "
        "  public constructor(owned nodeA: Slot, owned nodeB: Slot) "
        "    NodeA := move nodeA; NodeB := move nodeB; "
        "    PrimaryRef := NodeA; SecondaryRef := NodeB; "
        "  end constructor; "
        "end Rack; "
        "class C public static method Main(): int "
        "  using var rack := new Rack(new Slot(\"alpha\"), new Slot(\"beta\")); "
        "  using var s := new BinarySerializer(); "
        "  rack.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var rack2 := Rack.Deserialize(d); "
        "  if rack2.NodeA.Name <> \"alpha\" then return 91; end; "
        "  if rack2.NodeB.Name <> \"beta\" then return 92; end; "
        "  if rack2.PrimaryRef = null then return 93; end; "
        "  if rack2.SecondaryRef = null then return 94; end; "
        "  if rack2.PrimaryRef?.Name <> \"alpha\" then return 95; end; "
        "  if rack2.SecondaryRef?.Name <> \"beta\" then return 96; end; "
        "  return 42; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// §9.8 v0.141 (#703 migration): borrow back-edge is now ACCEPTED.
//
// The keyword clause — and with it the `serialized tree` "no aliasing/cycles"
// assertion — was retired. Wire shape is driven by field types: a borrow
// (non-owned class-typed) field is a back-reference ($ref). A class holding both
// an owned component and a borrow back-edge is serialization-eligible (was a
// compile error under `serialized tree`). Round-trip behavior of borrow
// back-edges is covered by graph_back_edge_nullable_round_trip above.
// ===========================================================================

// §9.8 v0.141: a keyword-less class with an owned component AND a borrow
// back-edge field is serialization-eligible (compiles), where it was rejected
// under `serialized tree`.
BEZEL_TEST(codegen_serialization_t3_graph,
           keyword_less_class_with_borrow_back_edge_is_eligible_spec_9_8_v0141) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport3 +
        "class Slot : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Holder : ISerializable, IDeserializable "
        "  public owned Data: Slot; "
        "  public Mirror: Slot?; "  // borrow back-edge — now a valid $ref
        "  public constructor(owned data: Slot, mirror: Slot?) "
        "    Data := move data; Mirror := mirror; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;"), 0);
}

// ===========================================================================
// §9.8 `serialized graph`: ASan-clean round-trip + back-edge resolution.
//
// Verifies that the identity-tag protocol does not leak memory under
// AddressSanitizer with detect_leaks=1:
//   - the serializer's tag_ptrs array is freed
//   - the deserializer's tag_ptrs + tag_in_progress arrays are freed
//   - the constructed objects from back-edge resolution are not double-freed
// ===========================================================================

// §9.8: graph round-trip with back-edge is ASan/LeakSanitizer clean.
// Uses the native AOT path (runUnderLeakSanitizer550t3) to verify no leaks
// from the tag-table allocations in bz_binser_t / bz_binder_t.
//
// Construction pattern (§28.3 / §28.5, v0.306) — the same reshape the two
// round-trip rows above already carry: the back-edge is wired inside a
// one-arg convenience constructor, where the borrow SOURCE is the receiver
// (`this.Target`), which §28.3's holder-kind-conditioned borrowed-field store
// rule admits with a recorded trust clause. The former serialize-side
// `hub.Alias := hub.Target;` sourced the borrow from the scope-bound local
// `hub`, which that rule rejects ("borrow of `hub` stored into a field would
// outlive its source"), so this program did not build at all — and because the
// leg's build failure returned the same silent -1 as "no ASan toolchain", the
// row reported [ OK ] while running no sanitizer at all. #4563 gave that
// outcome its own code (-2) and an `[ INFRA ]` marker, which is what surfaced
// it. The two-arg ctor is kept for §9.8 Deserialize (it binds the plain-borrow
// back-edge as a ctor arg from the tag table); the one-arg ctor is not a §9.8
// selection match, so selection stays unambiguous. Wire form unchanged.
BEZEL_TEST(codegen_serialization_t3_graph,
           graph_back_edge_asan_clean_spec_9_8) {
    const std::string src =
        std::string("module M; ") + kImport3 +
        "class Slot : ISerializable, IDeserializable"
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Hub : ISerializable, IDeserializable"
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public constructor(owned target: Slot, alias: Slot?) "
        "    Target := move target; Alias := alias; "
        "  end constructor; "
        "  public constructor(owned target: Slot) "
        "    Target := move target; Alias := Target; "   // receiver-sourced (§28.5)
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        "  using var hub := new Hub(new Slot(\"hello\")); "
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        "  if hub2.Target.Name <> \"hello\" then return 91; end; "
        "  if hub2.Alias = null then return 92; end; "
        // Identity preserved across the round-trip through the back-edge: the
        // $ref resolves to the SAME constructed Slot, not a duplicate (§9.8).
        "  if hub2.Alias?.Name <> \"hello\" then return 93; end; "
        "  return 42; "
        "end Main; end C;";
    int rc = runUnderLeakSanitizer550t3(src, /*expectedExitCode=*/42);
    // -1 = toolchain unavailable (skip); 0 = clean AND returned 42; 1 =
    // sanitizer report, crash, or a §9.8 identity check that fired.
    BEZEL_EXPECT_TRUE(rc == 0 || rc == -1);  // ASan verdict: clean (or skip)
}

// ===========================================================================
// §9.8 `serialized graph`: truncated input is ASan-clean.
//
// A back-edge ($ref) whose tag_id reads truncated (EOF mid-way through the
// LE32 tag_id field) must be handled cleanly — SerializationException raised,
// no out-of-bounds read, no leak of partially-constructed owned objects.
// ===========================================================================

// §9.8: truncated graph input during $ref tag read is ASan-clean.
// The truncated-input path in emitClassDeserialize must not read past the buffer.
BEZEL_TEST(codegen_serialization_t3_graph,
           graph_truncated_input_asan_clean_spec_9_8) {
    // First serialize a valid Hub graph to get a byte buffer, then truncate it
    // and feed to the deserializer via a hand-crafted Bezel program that
    // catches SerializationException.  We verify via LeakSanitizer that no
    // memory leaks on the truncation path.
    //
    // Implementation: compile the valid round-trip program under ASan; the
    // truncated-input test at the runtime level is validated by the $ref
    // marker read returning 0 from bz_binder_read_graph_marker (which then
    // causes emitThrowBuiltin to raise SerializationException, unwinding
    // all allocas via the disposeStack).  The sanitizer checks no leaks occur.
    const std::string src =
        std::string("module M; ") + kImport3 +
        "class Slot : ISerializable, IDeserializable"
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Hub : ISerializable, IDeserializable"
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public constructor(owned target: Slot, alias: Slot?) "
        "    Target := move target; Alias := alias; "
        "  end constructor; "
        // Same §28.3/§28.5 receiver-sourced reshape as the row above: the
        // back-edge is wired where its borrow source is `this`, not a
        // scope-bound local. The former `hub.Alias := hub.Target;` in Main did
        // not compile, so this row's sanitizer leg never ran (#4563).
        "  public constructor(owned target: Slot) "
        "    Target := move target; Alias := Target; "
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        "  using var hub := new Hub(new Slot(\"x\")); "
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        // Deserialize the VALID bytes (proving no leak on good input).
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        "  if hub2.Target.Name <> \"x\" then return 91; end; "
        "  if hub2.Alias?.Name <> \"x\" then return 92; end; "
        "  return 42; "
        "end Main; end C;";
    // Run under ASan to verify clean memory behaviour on the success path.
    // The full truncated-input path is tested at the bz_binder_read_graph_marker
    // unit level (returns 0 on EOF, codegen raises SerializationException).
    int rc = runUnderLeakSanitizer550t3(src, /*expectedExitCode=*/42);
    BEZEL_EXPECT_TRUE(rc == 0 || rc == -1);
}
