// Codegen — issue #2460 / spec §9.8 ("Serialization synthesis") + stdlib
// §2.15 ("Concrete serialization formats") + language spec §28.3/§28.9/
// §28.13, docs revision v0.235.
//
// Found while auditing the family for #2397 (graph-mode component-slot
// disposal-on-unwind, `emitClassDeserialize`'s `disposeStack_` scope pushed
// near the top of the function). That fix correctly excludes a graph-mode
// plain-borrow back-edge component (`comp.isBackEdge`) from disposal — its
// slot holds a BORROWED pointer (resolved via `bz_binder_graph_resolve_tag`),
// not an owned reference, so disposing it would free a value another
// component/handle still owns.
//
// THE BUG. The SAME function had two OTHER, independent, pre-existing manual
// "dispose already-filled slots" loops (LlvmBackend_Serialization.cpp) that
// did NOT apply the same exclusion:
//   - the truncated-field-name `errBB` block (bz_binder_read_field_name
//     returned null without a pending exception);
//   - the missing-required-field `missBB` block (EndObject completed but a
//     required component's seen-flag was never set).
// Both classified a slot as disposable purely by `ty->kind == Named &&
// ty->declaration && ty->declaration->kind == Class` — true for a genuinely
// owned class component AND for a graph-mode back-edge component (e.g.
// `Alias: Slot?`), since a back-edge's resolved type is a real class
// `TypeSymbol` just like an owned field's (unlike `Shared<T>`/`Weak<T>`,
// which have no `declaration` and are — separately — simply never disposed
// by either loop, a leak, not a crash). Neither loop checked
// `comp.isBackEdge`. When a back-edge resolved successfully (aliasing an
// EARLIER owned component's already-registered object) and a LATER required
// field was then found missing, the missing-required-field loop disposed
// BOTH the owned component (correctly) AND its back-edge alias (the SAME
// pointer) — a double-free, empirically confirmed as a heap-use-after-free
// under ASan.
//
// THE FIX. Both manual loops are deleted outright rather than patched with a
// third `!comp.isBackEdge` check: #2397 (a few dozen lines above, in the
// SAME function) already built the correct per-component dispatch — a
// `disposeStack_` scope registered UNCONDITIONALLY before any field is read,
// classifying Shared<C> -> bz_shared_release, owned Weak<C> ->
// bz_weak_release, back-edge -> no disposal, owned-class/string -> the same
// mechanisms these two manual loops used. `emitThrowBuiltin` (called by both
// blocks) unconditionally calls `emitUnwindDisposals()`, which sweeps that
// SAME scope — already armed, already correctly classified — so it alone is
// sufficient for every throw site in this function. Three independently
// maintained copies of the same classification (this fix's own #2397
// registration, plus these two manual loops) is the shape that let the
// back-edge exclusion drift out of sync in the first place; collapsing to
// the one already-fixed, already-tested mechanism removes the duplication
// instead of adding a fourth parallel patch.
//
// VERIFICATION. The missing-required-field variant below is EMPIRICALLY
// CONFIRMED: under ASan (scratch-copy diff -> git checkout revert -> rebuild
// -> confirm heap-use-after-free -> reapply -> confirm clean), the exact
// repro in this file crashed with "AddressSanitizer: heap-use-after-free ...
// READ of size 8" (Alias's slot read after Target's identical pointer was
// already freed) before the fix, and runs clean (LSan-clean, exit 42) after.
// The `errBB` (truncated-field-name) variant has the IDENTICAL code shape
// and is fixed by the SAME deletion, but was NOT independently reproduced:
// per the surrounding code's own comment, `bz_binder_read_field_name`
// returns null without a pending exception "only for the OOM
// allocation-failure paths" since the #2403 fix intercepts ordinary
// truncation earlier (via `emitPendingCheck()`); one non-OOM attempt (a
// trailing comma hoping to desync the JSON pre-count scan) did not reach
// that path. The fix does not depend on that path being reachable by a
// crafted input — `emitUnwindDisposals()` covers it unconditionally exactly
// like the confirmed variant — but this file records the distinction rather
// than claiming a repro that was not obtained.
//
// APIs verified against Bezel_Standard_Library.md: ISerializable/
// IDeserializable (§2.14), JsonDeserializer (§2.15, `constructor(input:
// string)`), SerializationException (§2.15).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

const char* kImport = "import System.Serialization; ";

}  // namespace

// ===========================================================================
// EMPIRICALLY CONFIRMED: Target ($id:1, owned) and Alias ($ref:1, a
// back-edge aliasing the SAME object) both read successfully; the REQUIRED
// `Second` field is absent from the wire entirely, firing the
// missing-required-field loop. Pre-fix: heap-use-after-free (Alias's slot,
// the same pointer as the already-freed Target, read/freed a second time).
// stdlib §2.15 / §9.8 / §28.3 / §28.13.
// ===========================================================================
BEZEL_TEST(serialization_manual_loop_backedge_double_free_2460,
           missing_required_field_backedge_no_double_free_spec_9_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Slot : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Leaf : ISerializable, IDeserializable "
        "  public property Val: string; "
        "  public constructor(val: string) Val := val; end constructor; "
        "end Leaf; "
        "class Hub6 : ISerializable, IDeserializable "
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public owned Second: Leaf; "
        "  public constructor(owned target: Slot, alias: Slot?, owned second: Leaf) "
        "    Target := target; Alias := alias; Second := second; "
        "  end constructor; "
        "end Hub6; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"Target\\\":{\\\"$id\\\":1,\\\"Name\\\":\\\"t\\\"},\\\"Alias\\\":{\\\"$ref\\\":1}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var h := Hub6.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);         // no UAF / double-free (pre-fix: ASan abort)
    BEZEL_EXPECT_EQ(r.exitCode, 42);  // the missing-required-field error is still raised
}

// ===========================================================================
// Control: the SAME shape, but with the back-edge NEVER resolved (Alias is
// simply absent from the wire) -- confirms the fix does not disturb the
// ordinary missing-required-field path when no back-edge is in play.
// stdlib §2.15 / §9.8.
// ===========================================================================
BEZEL_TEST(serialization_manual_loop_backedge_double_free_2460,
           missing_required_field_no_backedge_control_spec_9_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Slot : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Leaf : ISerializable, IDeserializable "
        "  public property Val: string; "
        "  public constructor(val: string) Val := val; end constructor; "
        "end Leaf; "
        "class Hub6 : ISerializable, IDeserializable "
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public owned Second: Leaf; "
        "  public constructor(owned target: Slot, alias: Slot?, owned second: Leaf) "
        "    Target := target; Alias := alias; Second := second; "
        "  end constructor; "
        "end Hub6; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"Target\\\":{\\\"$id\\\":1,\\\"Name\\\":\\\"t\\\"}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var h := Hub6.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);         // Target alone disposed cleanly, no leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Clean-round-trip control: a WELL-FORMED input with a resolving back-edge
// and every required field present must still construct correctly and
// dispose nothing prematurely -- the deletion of both manual loops must not
// disturb the happy path (they never fired there before either, but this
// pins that removing them leaves the successful-construction path
// unaffected). stdlib §2.15 / §9.8.
// ===========================================================================
BEZEL_TEST(serialization_manual_loop_backedge_double_free_2460,
           clean_construction_with_backedge_unaffected_spec_9_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Slot : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Leaf : ISerializable, IDeserializable "
        "  public property Val: string; "
        "  public constructor(val: string) Val := val; end constructor; "
        "end Leaf; "
        "class Hub6 : ISerializable, IDeserializable "
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public owned Second: Leaf; "
        "  public constructor(owned target: Slot, alias: Slot?, owned second: Leaf) "
        "    Target := target; Alias := alias; Second := second; "
        "  end constructor; "
        "end Hub6; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"Target\\\":{\\\"$id\\\":1,\\\"Name\\\":\\\"t\\\"},\\\"Alias\\\":{\\\"$ref\\\":1},\\\"Second\\\":{\\\"$id\\\":2,\\\"Val\\\":\\\"z\\\"}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  using var h := Hub6.Deserialize(d); "
        "  if h.Target.Name = \"t\" and h.Alias?.Name = \"t\" and h.Second.Val = \"z\" then "
        "    return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
