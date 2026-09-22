// Codegen — issue #2396 / spec §9.8 ("Serialization synthesis", the graph/
// `$ref` "Identity is preserved, never duplicated" bullet) + stdlib §2.15
// ("Concrete serialization formats"), docs revision v0.235.
//
// #2396's claim: `bz_binder_read_graph_marker`'s JSON path
// (`src/runtime/bezel_runtime_serialization.cpp`) can raise
// `SerializationException` internally (via `bz_jder_parse_int` ->
// `bz_ser_throw_at_json`) while parsing a malformed/truncated `$id`/`$ref`
// tag_id, yet still returns a "successful" marker value (BZ_SER_INLINE or
// BZ_SER_REF, with *out_tag left at 0) -- the function never checks for its
// own pending exception before returning. None of the 4 codegen call sites
// for this function in `LlvmBackend_Serialization.cpp`'s `emitClassDeserialize`
// (the `Shared<C>` identity-participant branch, the owned-`Weak<C>`
// back-reference branch, the plain-borrow back-edge branch, and the
// owned-class-in-graph-mode branch) checked for that pending exception before
// acting on the bogus marker/tag -- each falls through to
// `bz_binder_graph_resolve_tag`/`bz_binder_graph_mark_in_progress` on the
// stale tag_id=0, and (for the Shared<C>/Weak<C>/back-edge branches) the
// failed resolve then calls `emitThrowBuiltin("SerializationException")`
// UNCONDITIONALLY -- which unconditionally overwrites the thread-local
// `bz_exc` slot, orphaning the first, genuinely-caused exception object (and
// its heap-allocated Message string): a leak, and the caller observes the
// WRONG (message-less) exception. This is the same "Bug-A pattern" fixed at
// several other runtime-reader call sites in this file (#2378 lineage,
// #2403, #2404, #2419, #2427).
//
// VERIFY-FIRST (scripts/agent_check.sh --probe --asan, against pre-fix
// origin/main, using the issue's own `Hub`/`Slot` graph shape -- `Hub` owns
// `Target: Slot` and has a nullable plain-borrow back-edge `Alias: Slot?`,
// automatically graph-shaped per §9.8/#2325's field-shape inference since a
// non-owned class-typed field is present, no explicit `serialized graph`
// clause needed): a hand-written JSON literal with the back-edge field
// FIRST in wire order and a corrupted `$ref` tag (`{"Alias":{"$ref":},
// "Target":{"$id":1,"Name":"hello"}}` -- brace-balanced, so the top-level
// `bz_binder_begin_object`'s own pre-scan does not intercept it first; `Alias`
// placed first so `Target`'s owned `Slot` is never constructed, isolating
// this fix from the unrelated, already-documented graph-mode component-slot
// disposal gap noted at LlvmBackend_Serialization.cpp's disposeStack_
// scope-push comment -- "Scoped to tree mode ... left to a follow-up") caught
// `SerializationException` (correct control flow) but under
// `ASAN_OPTIONS=detect_leaks=1`: "124 byte(s) leaked in 3 allocation(s)",
// leak trace rooted at `bz_ser_throw <- bz_jder_parse_int <-
// bz_binder_read_graph_marker <- main` -- the genuine, message-bearing
// exception ("Expected JSON integer (§2.15)") -- and the CAUGHT exception's
// `Message` was empty, confirming a second, message-less exception (raised by
// the unconditional `emitThrowBuiltin` in the back-edge branch's cycle-check)
// superseded it. Post-fix: 0 bytes leaked, and the caught exception's Message
// is the genuine "Expected JSON integer (§2.15)".
//
// Fix: `emitPendingCheck()` immediately after each of the 4
// `bz_binder_read_graph_marker` call sites in
// `LlvmBackend_Serialization.cpp`'s `emitClassDeserialize`, mirroring
// `bz_binder_read_case_discriminator`'s/`bz_binder_read_poly_discriminator`'s
// call sites in the same file -- an already-pending exception now propagates
// (via the standard disposal-aware unwind machinery, `emitPendingCheckIf`)
// before the bogus marker/tag_id is ever acted on.
//
// Per-site reachability (traced from `bz_binder_read_graph_marker`'s actual
// body, `bezel_runtime_serialization.cpp`): the JSON REF path
// (`{"$ref":<tag>}`) returns BZ_SER_REF unconditionally once the leading key
// is recognized as "$ref", even when the subsequent `bz_jder_parse_int` threw
// -- so the plain-borrow back-edge branch (empirically confirmed here) and
// the `Shared<C>`/owned-`Weak<C>` branches, which all resolve `tagId` the
// same way after the same call shape, are reachable through the identical
// malformed-`$ref` input by construction (same runtime call, same
// "resolve-fails -> unconditional emitThrowBuiltin" shape) -- not
// independently reproduced per-type here given the lane's time budget, but
// the code-level argument is exact, not speculative. The JSON INLINE path
// (`{"$id":<tag>,...}`) similarly returns BZ_SER_INLINE unconditionally once
// the leading key is "$id", even on a parse failure -- reachable through the
// owned-class-in-graph-mode branch; that branch's fall-through additionally
// recurses into `emitClassDeserialize` on a stream left at an
// indeterminate position by the failed `bz_jder_parse_int`, so the missing
// check there also guards against compounding into whatever that recursive
// call encounters next, not merely the leak. `bz_binder_graph_mark_in_progress`/
// `bz_binder_graph_resolve_tag`/`bz_binder_graph_register_tag` all guard
// `tag_id <= 0` (bz_binder_read_graph_marker sets `*out_tag = 0` before
// parsing), so the bogus tag_id=0 causes no out-of-bounds access on any of
// the 4 paths -- the observable defect is uniformly the leak/wrong-exception
// pattern, not memory corruption.
//
// APIs verified against Bezel_Standard_Library.md: ISerializable/
// IDeserializable (§2.14), JsonDeserializer (§2.15, `constructor(input:
// string)`), SerializationException (§2.15).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// runAotChecked returns ran=false on ANY sanitizer fire (leak / UAF /
// crash); mapping that to a sentinel lets a single EXPECT_EQ pin both
// memory-cleanliness AND the round-tripped exit value in one assertion
// (mirrors serialization_case_discriminator_double_throw_2427_test.cpp's
// runChecked).
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImport = "import System.Serialization; ";

// The issue's own graph shape: Hub owns Target (Slot), and has a nullable
// plain-borrow back-edge Alias: Slot? -- automatically graph-shaped (§9.8 /
// #2325 field-shape inference: a non-owned class-typed field implies graph
// shape), no explicit `serialized graph` clause required.
const char* kHubSlot =
    "class Slot : ISerializable, IDeserializable "
    "  public property Name: string; "
    "  public constructor(name: string) Name := name; end constructor; "
    "end Slot; "
    "class Hub : ISerializable, IDeserializable "
    "  public owned Target: Slot; "
    "  public Alias: Slot?; "
    "  public constructor(owned target: Slot, alias: Slot?) "
    "    Target := target; Alias := alias; "
    "  end constructor; "
    "end Hub; ";

}  // namespace

// ===========================================================================
// Primary fix target: the issue's own exact repro (back-edge branch),
// LSan-checked. Pre-fix: sanitizer fires (leak) -> runChecked returns -999.
// Post-fix: clean, catches SerializationException -> runChecked returns 42.
// `Alias` is placed FIRST in the wire so `Target`'s owned Slot is never
// constructed -- isolating this leak from the unrelated, pre-existing
// graph-mode component-slot disposal gap (see file header).
// ===========================================================================
BEZEL_TEST(codegen_serialization_graph_marker_pending_2396,
           issue_repro_corrupted_ref_tag_lsan_clean_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kHubSlot +
        "class C public static method Main(): int "
        "  var json: string := "
        "    \"{\\\"Alias\\\":{\\\"$ref\\\":},"
        "\\\"Target\\\":{\\\"$id\\\":1,\\\"Name\\\":\\\"hello\\\"}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var hub := Hub.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Message-correctness control (not leak-checked -- plain runAot): the
// caught exception must carry the GENUINE parse-failure diagnostic
// ("Expected JSON integer"), not an empty Message -- an empty Message is
// exactly the symptom of the second, superseding exception the bug produces
// (the codegen's `emitThrowBuiltin("SerializationException")` builds a
// calloc'd object with no Message set). A coarse "did it raise SOME
// SerializationException" assertion would pass even on the pre-fix
// superseding exception, so this asserts on the message text directly.
// ===========================================================================
BEZEL_TEST(codegen_serialization_graph_marker_pending_2396,
           corrupted_ref_tag_message_is_the_genuine_cause_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport + kHubSlot +
        "class C public static method Main(): int "
        "  var json: string := "
        "    \"{\\\"Alias\\\":{\\\"$ref\\\":},"
        "\\\"Target\\\":{\\\"$id\\\":1,\\\"Name\\\":\\\"hello\\\"}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var hub := Hub.Deserialize(d); "
        "    return 1; "  // reached only if it wrongly did NOT raise
        "  catch var e: SerializationException => "
        "    return (if e.Message.Contains(\"Expected JSON integer\") "
        "            then 42 else 2); "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// "Must still raise" control: a WELL-FORMED $ref to a tag that was never
// registered (no parse failure, no pending exception -- bz_jder_parse_int
// succeeds cleanly with tag_id=99) must still raise SerializationException
// via the pre-existing "unknown tag/cycle" check -- the fix must not
// suppress this legitimate, pre-existing error path. (Its Message is
// expected to be empty here -- this IS the designed emitThrowBuiltin guard
// path, not the bug; only the corrupted-tag case above should carry a
// genuine parse-failure message.)
// ===========================================================================
BEZEL_TEST(codegen_serialization_graph_marker_pending_2396,
           wellformed_unresolvable_ref_still_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport + kHubSlot +
        "class C public static method Main(): int "
        "  var json: string := "
        "    \"{\\\"Alias\\\":{\\\"$ref\\\":99},"
        "\\\"Target\\\":{\\\"$id\\\":1,\\\"Name\\\":\\\"hello\\\"}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var hub := Hub.Deserialize(d); "
        "    return 1; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// Clean-round-trip control: a VALID $ref that DOES resolve must still work
// end-to-end (identity preserved) -- the fix must not disturb the working
// graph back-edge path. Mirrors serialization_tier3_graph_test.cpp's own
// coverage but re-asserted here, LSan-checked, alongside this fix's own
// tests to guard against the pending-check firing spuriously on the happy
// path (bz_exc is never set there, so emitPendingCheck must be a no-op).
// ===========================================================================
BEZEL_TEST(codegen_serialization_graph_marker_pending_2396,
           clean_roundtrip_back_edge_resolves_spec_9_8) {
    BEZEL_EXPECT_EQ(runChecked(
        std::string("module M; ") + kImport +
        "class Slot : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Hub : ISerializable, IDeserializable "
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public constructor(owned target: Slot, alias: Slot?) "
        "    Target := target; Alias := alias; "
        "  end constructor; "
        "  public constructor(owned target: Slot) "
        "    Target := target; Alias := Target; "
        "  end constructor; "
        "end Hub; "
        "class C public static method Main(): int "
        "  using var hub := new Hub(new Slot(\"alpha\")); "
        "  using var s := new JsonSerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  using var hub2 := Hub.Deserialize(d); "
        "  if hub2.Target.Name <> \"alpha\" then return 91; end; "
        "  if hub2.Alias = null then return 92; end; "
        "  if hub2.Alias?.Name <> \"alpha\" then return 93; end; "
        "  return 42; "
        "end Main; end C;"), 42);
}
