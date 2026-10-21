// Codegen — issue #2397 / spec §9.8 ("Serialization synthesis") + stdlib
// §2.15 ("Concrete serialization formats") + language spec §28.3/§28.9/
// §28.13, docs revision v0.235.
//
// #2378 fixed a leak in `emitClassDeserialize` (LlvmBackend_Serialization.cpp):
// a component slot already populated by an EARLIER successful field read
// (e.g. `First: string`) had no disposal registered anywhere, so a LATER
// field's read that recurses into another `emitClassDeserialize` call and
// throws correctly and promptly orphaned the already-read value. The fix
// pushed a dedicated `disposeStack_` scope around the per-field loop,
// registering each owned-class/string component's slot into it up front.
// That fix was explicitly scoped `if (!isGraphMode)` (tree mode only): a
// `Shared<T>`/`Weak<T>` slot holds a HANDLE (from `bz_shared_new`/
// `bz_shared_clone`/`bz_shared_downgrade`), not a plain class instance or a
// string, and reusing the generic vtable dispatch / `bz_string_release` on it
// would be a wrong-dispatch regression — left for #2397 as a dedicated pass.
//
// #2397's claim (verified here): NONE of graph mode's four per-field value
// shapes registered their component's slot for unwind-disposal at all — the
// SAME leak shape #2378 closed for tree mode was still open for graph mode.
//
// THE FIX (LlvmBackend_Serialization.cpp, the disposeStack_ scope pushed near
// the top of `emitClassDeserialize`): the blanket `!isGraphMode` gate is
// replaced with a per-component dispatch, in the SAME order the real
// per-field value loop (a few hundred lines below in the same function)
// already tests these shapes, so the two decisions stay in lockstep:
//   1. Shared<C> identity participant (handledSharedElementClass) — a
//      `Disposal` entry with `unwindOnly=true, sharedHandle=true`, dispatched
//      via `bz_shared_release` by the PRE-EXISTING `it->sharedHandle` branch
//      in `emitDisposals` (LlvmBackend_Disposal.cpp) — the same mechanism a
//      `using var` bound to `Shared<T>` already uses (§28.13 / stdlib §2.16).
//   2. owned Weak<C> back-reference (handledWeakElementClass, gated on
//      comp.isOwned) — `unwindOnly=true, weakHandle=true` →
//      `bz_weak_release`, the `it->weakHandle` branch.
//   3. plain-borrow back-edge (comp.isBackEdge) — a BORROWED pointer
//      (resolved via bz_binder_graph_resolve_tag): this component owns
//      NOTHING, so NO disposal is registered — registering the generic
//      vtable dispatch here would premature-free a value another live
//      component/handle still references.
//   4. owned-class-in-graph-mode (comp.isOwned, a plain Named class, not
//      Shared/Weak) — falls through to the SAME generic-vtable-dispatch
//      branch tree mode already used (unchanged mechanism, just no longer
//      excluded by the blanket gate).
//   A plain string component (never identity-tagged, §9.8) is likewise no
//   longer excluded in graph mode — the same `stringRelease` disposal tree
//   mode already registers.
//
// TESTS BELOW (each FAILS — an LSan leak report — without the fix and PASSES
// with it; mechanically verified: reverting the LlvmBackend_Serialization.cpp
// hunk and rebuilding reproduces each leak below exactly, byte-for-byte;
// reapplying clears it):
//   (A) Shared<C> — a Shared<Inner> field reads successfully ($id), a LATER
//       sibling field's read throws (malformed graph-identity marker) —
//       pre-fix leaks the bz_shared_t handle (16 bytes, bz_shared_new).
//   (B) owned Weak<C> — a Shared<Inner> (registers the tag) AND an owned
//       Weak<Inner> (resolves $ref, bz_shared_downgrade) both read
//       successfully, a LATER field throws — pre-fix leaks the bz_weak_t
//       handle (16 bytes, bz_shared_downgrade).
//   (C) plain string in graph mode — a plain `Name: string` field reads
//       successfully in a graph-mode class (forced graph-shaped by an
//       accompanying `Shared<Inner>` field), the Shared field's OWN read then
//       throws — pre-fix leaks the "hello" bz.string (38 bytes: 32-byte
//       header + 6-byte UTF-8 data, bz_binder_read_string).
//   (D) owned-class-in-graph-mode + string, binary truncation (mirrors
//       #2378's own tree-mode methodology exactly, now in graph mode): a
//       plain `Label: string` field reads successfully, THEN the recursive
//       `emitClassDeserialize` call for `owned Target: Slot` (graph-shaped by
//       the accompanying back-edge `Alias: Slot?`) throws from a truncated
//       nested field read — pre-fix leaks Label's "thelabel" bz.string
//       (41 bytes: 32-byte header + 9-byte UTF-8 data,
//       bz_binder_read_bz_string).
//   (E) tree-mode regression control — #2378's own fixed mechanism (a plain
//       tree-mode class, no graph shape at all) must remain unaffected by
//       this fix's restructuring of the same disposeStack_ registration loop.
//
// Adversarial note (recorded, not a defect): every hand-written JSON literal
// below is a SINGLE, unconcatenated string literal. Splitting a literal
// across lines with `+` makes the bound local a `+1` PRODUCER
// (isPlusOneStringProducer's BinaryExpr `+` case, LlvmBackend_Setup.cpp) that
// ESCAPES when passed as a constructor argument — excluded from the
// escape-walk's scope-end release by the project's own documented "absence
// from stringReleaseEligible_ ⇒ leaks, the pre-existing, safe direction"
// policy (LlvmBackend_impl.hpp, stringReleaseEligible_'s comment). That is
// UNRELATED to #2397 and was confirmed identical on unmodified origin/main
// (verified during this fix's investigation) — recorded here only so a
// future reader of a `+`-joined variant of these tests does not mistake that
// artifact for a regression in this fix.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kImport = "import System.Serialization; ";

}  // namespace

// ===========================================================================
// (A) Shared<C> identity-participant handle — released via bz_shared_release
// on unwind, not orphaned. stdlib §2.15 / §28.13 / §9.8.
// ===========================================================================
BEZEL_TEST(serialization_graph_slot_disposal_2397,
           shared_handle_released_on_later_throw_spec_9_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Inner : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Inner; "
        "class Leaf : ISerializable, IDeserializable "
        "  public property Val: string; "
        "  public constructor(val: string) Val := val; end constructor; "
        "end Leaf; "
        "class Holder : ISerializable, IDeserializable "
        "  public owned Handle: Shared<Inner>; "
        "  public owned Second: Leaf; "
        "  public constructor(owned handle: Shared<Inner>, owned second: Leaf) "
        "    Handle := handle; Second := second; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"Handle\\\":{\\\"$id\\\":1,\\\"V\\\":7},\\\"Second\\\":{}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var h := Holder.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // LSan-clean: the Shared handle released exactly once
    BEZEL_EXPECT_EQ(r.exitCode, 42);  // control flow: the throw is still caught correctly
}

// ===========================================================================
// (B) owned Weak<C> back-reference handle — released via bz_weak_release on
// unwind, not orphaned. stdlib §2.15 / §28.13 / §9.8.
// ===========================================================================
BEZEL_TEST(serialization_graph_slot_disposal_2397,
           weak_handle_released_on_later_throw_spec_9_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Inner : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Inner; "
        "class Leaf : ISerializable, IDeserializable "
        "  public property Val: string; "
        "  public constructor(val: string) Val := val; end constructor; "
        "end Leaf; "
        "class Holder : ISerializable, IDeserializable "
        "  public owned Strong: Shared<Inner>; "
        "  public owned Weakref: Weak<Inner>; "
        "  public owned Second: Leaf; "
        "  public constructor(owned strong: Shared<Inner>, owned weakref: Weak<Inner>, "
        "                     owned second: Leaf) "
        "    Strong := strong; Weakref := weakref; Second := second; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"Strong\\\":{\\\"$id\\\":1,\\\"V\\\":7},\\\"Weakref\\\":{\\\"$ref\\\":1},\\\"Second\\\":{}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var h := Holder.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // LSan-clean: both the Shared and Weak handles released
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (C) A plain string component in a graph-shaped class — released via
// bz_string_release on unwind, not orphaned (the blanket !isGraphMode gate
// used to exclude strings too, not only class-typed components). stdlib
// §2.15 / §4.6 / §28.9 / §9.8.
// ===========================================================================
BEZEL_TEST(serialization_graph_slot_disposal_2397,
           plain_string_in_graph_mode_released_on_later_throw_spec_9_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Inner : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Inner; "
        "class Holder4 : ISerializable, IDeserializable "
        "  public Name: string; "
        "  public owned Handle: Shared<Inner>; "
        "  public constructor(name: string, owned handle: Shared<Inner>) "
        "    Name := name; Handle := handle; "
        "  end constructor; "
        "end Holder4; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"Name\\\":\\\"hello\\\",\\\"Handle\\\":{}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var h := Holder4.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // LSan-clean: Name's string released exactly once
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (D) owned-class-in-graph-mode + a plain string sibling, via BINARY
// truncation (mirrors #2378's own tree-mode methodology): Label (string)
// reads successfully, then the recursive emitClassDeserialize call for
// `owned Target: Slot` throws from a truncated nested field read. Graph
// shape is forced by the accompanying back-edge `Alias: Slot?`. A REAL
// Serialize() round trip (not a hand-written literal) is truncated via
// AsReadOnlySpan, exactly like #2378's binary repro. stdlib §2.15 / §9.8 /
// §28.3 / §28.9.
// ===========================================================================
BEZEL_TEST(serialization_graph_slot_disposal_2397,
           owned_class_and_string_released_on_binary_truncation_spec_9_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Slot : ISerializable, IDeserializable "
        "  public property Name: string; "
        "  public constructor(name: string) Name := name; end constructor; "
        "end Slot; "
        "class Hub5 : ISerializable, IDeserializable "
        "  public Label: string; "
        "  public owned Target: Slot; "
        "  public Alias: Slot?; "
        "  public constructor(label: string, owned target: Slot, alias: Slot?) "
        "    Label := label; Target := target; Alias := alias; "
        "  end constructor; "
        "end Hub5; "
        "class C public static method Main(): int "
        "  var target := new Slot(\"targetname\"); "
        "  using var hub := new Hub5(\"thelabel\", move target, null); "
        "  using var s := new BinarySerializer(); "
        "  hub.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var fullLen: int := bytes.Length; "
        "  var cut: int := (fullLen * 3) / 5; "
        "  if cut < 1 then cut := 1; end; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cut)); "
        "  try "
        "    using var h := Hub5.Deserialize(d); "
        "    return 0; "  // did not truncate meaningfully -- not the intended shape
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // LSan-clean: Label's string released exactly once
    BEZEL_EXPECT_EQ(r.exitCode, 42);  // the truncation must still raise correctly
}

// ===========================================================================
// (E) Tree-mode regression control — #2378's own original fix (a plain
// tree-mode class: no Shared/Weak/back-edge field at all) must be unaffected
// by this fix restructuring the SAME disposeStack_ registration loop from a
// blanket `if (!isGraphMode)` gate into a per-component dispatch. §9.8 /
// §28.3 / §28.9.
// ===========================================================================
BEZEL_TEST(serialization_graph_slot_disposal_2397,
           tree_mode_2378_mechanism_unaffected_spec_9_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport +
        "class Leaf : ISerializable, IDeserializable "
        "  public property First: string; "
        "  public constructor(first: string) First := first; end constructor; "
        "end Leaf; "
        "class Pair : ISerializable, IDeserializable "
        "  public First: string; "
        "  public owned Second: Leaf; "
        "  public constructor(first: string, owned second: Leaf) "
        "    First := first; Second := second; "
        "  end constructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"First\\\":\\\"hello\\\",\\\"Second\\\":{}}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  try "
        "    using var h := Pair.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 42; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);         // LSan-clean: #2378's tree-mode fix still works
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
