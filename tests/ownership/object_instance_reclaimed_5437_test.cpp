// §28.9 / §28.2 / §8.1 / §8.6 / §3.4 (issue #5437): `new Object()` is a
// TRIVIAL-DISPOSE CLASS ALLOCATION, and the compiler inserts its free at the
// holder's release point like any other.
//
// §8.1 declares `Object` with no destructor and no fields, so §28.9's
// trivial-dispose definition is met clause by clause: no destructor, no `owned`
// fields, no owning-inline-array fields, and no base class at all (Object is the
// root — §3.4's keyword alias for the built-in class every reference type
// reaches). §28.9 then requires the compiler to insert the free, and the
// must-use ceremony exemption (K1.6) is why that free is SILENT — it runs no
// user code, so there is nothing for a `using var` to announce. §28.2's
// "`Dispose()` is not on `Object`" is about the METHOD surface, not about the
// free: the instance still has to be released.
//
// THE DEFECT. Nothing released it. `Object` resolves to
// `sema::PrimitiveType::Object` (a `TypeRef::Kind::Primitive`), and every
// disposal-registration arm keys on `Kind::Named` — `classOfLocal` returns a
// symbol only for Named, `isClassOrInterfaceRef` requires `rt->declaration`,
// `builtinTypeName`/`usingVarBuiltinHeadName` answer only for Named, and the
// #2992 end-of-chain guard's `isVtableLessBuiltinClassName` does not fire
// because `Object` is NOT vtable-less (it carries `bvt$Object`). So the holder
// registered NO disposal at all: the reclaim was absent from the generated code,
// not merely a no-op thunk. MEASURED: 8 bytes per allocation in every spelling.
//
// THE DISCRIMINATOR, and why it is in this file. `new Empty()` — an equally
// trivial-dispose USER class — was freed correctly all along. The pre-fix leak
// tracked the `new Object()` allocation itself, not trivial-dispose-ness, so
// `empty_control_still_freed_spec_28_9` below is the control that makes the
// `Object` rows attributable.
//
// THE SOUNDNESS BOUND (why these rows are not simply "free the slot"). §5.1
// makes `Object` the universal reference type, value types reaching it by
// BOXING (§8.6), so an `Object`-typed slot does NOT necessarily hold an
// `Object` instance — it can hold a §8.6 box. Boxes are runtime-managed
// (§28.9) and are reclaimed by their OWN release rule. A fix keyed on the static
// slot type would free a box the runtime still owns, which is a soundness
// regression, strictly worse than the leak. So the reclaim is guarded by a
// RUNTIME vtable-identity test, and `object_box_keeps_its_own_release_rule_8_6`
// / `object_slot_reassigned_to_box_keeps_box_8_6` pin that the guard holds: were
// it widened, the scope-end `bz_box_release` (#2060) would meet an already-freed
// wrapper and ASan would report a double-free / use-after-free.
//
// WHAT IS *NOT* PINNED HERE. `Type` is runtime-managed for a different reason —
// one interned token per type, never a user allocation — and its operator is not
// reachable from a running program at all (`typeof` is rejected in codegen,
// #487; `GetType` in sema, #511), so there is no `Object`-slot shape to exercise.
// §22.8's "an `owned` `Type` field is inadmissible" keeps `Type` out of the
// owned-transfer path this fix keys on.
//
// AMPLIFICATION. Every leak row loops 100×, so the pre-fix leak is 800 bytes in
// 100 allocations rather than 8 in 1 — an 8-byte-per-allocation leak must not be
// judged on a single run. The exit codes are the program's own `sink mod 256`
// observable (independent of the leak); all are < 256, so no 8-bit truncation.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "runAotChecked: SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// §28.9: `using var o := new Object()` — the issue's PRIMARY shape, in the same
// program as the `new Empty()` control so one run attributes the difference.
// 100 iterations → 800 bytes pre-fix. Without the fix LSan fires and `r.ran`
// goes false; with it the run is clean and the program returns 100.
const char* kUsingVarObject =
    "module M; "
    "class Empty end Empty; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    using var e := new Empty(); "
    "    using var o := new Object(); "
    "    sink := sink + 1; "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

// §28.9: the same holder spelled `var` with an EXPLICIT `Object` annotation. The
// annotation is the only difference from kUsingVarObject's spelling — the two
// must reach the same release point.
const char* kPlainVarObject =
    "module M; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    var o: Object := new Object(); "
    "    sink := sink + 1; "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

// §28.9: the INFERRED plain `var` spelling. No annotation anywhere, so nothing
// keys on a declared type — the allocation is the only signal.
const char* kInferredVarObject =
    "module M; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    var o := new Object(); "
    "    sink := sink + 1; "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

// §8.6 / §28.9: the SOUNDNESS BOUND, boxed value. `var o: Object := j` BOXES the
// int into an `Object`-typed slot; the box is runtime-managed and its wrapper is
// released by `bz_box_release` at scope end (#2060). The `Object` release must
// NOT free it. If the reclaim were unconditional, this row's scope-end
// `bz_box_release` would meet an already-freed wrapper and ASan would report it
// — so a GREEN row here is the evidence the guard is doing its job, and the row
// is run because the leak-oracle alone cannot tell "not freed by us" from
// "freed twice".
const char* kObjectSlotHoldsBox =
    "module M; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    var o: Object := j; "
    "    sink := sink + o.GetHashCode(); "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

// §8.6 / §28.9: the same bound reached by REPLACEMENT rather than by
// initialization — an `Object`-typed slot that held an `Object` INSTANCE and is
// then overwritten with a box. The instance is gone (freed or overwritten); the
// slot now holds runtime-managed memory the `Object` release must leave alone.
// This is the case a static slot-type test cannot get right and the runtime
// vtable-identity guard exists for.
const char* kObjectSlotReassignedToBox =
    "module M; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    var o: Object := new Object(); "
    "    o = j; "
    "    sink := sink + o.GetHashCode(); "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

// §28.9: the CONTROL. `new Empty()` is trivial-dispose on the same terms as
// `Object` (no destructor, no fields) and was ALWAYS freed correctly. If this row
// ever goes red the fix has broken ordinary trivial-dispose release — which is
// what makes the `Object` rows attributable to `Object` specifically rather than
// to trivial-dispose-ness in general.
const char* kEmptyControlOnly =
    "module M; "
    "class Empty end Empty; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    using var e := new Empty(); "
    "    sink := sink + 1; "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

}  // namespace

// §28.9 / §8.1 — `using var o := new Object()` is reclaimed at scope end.
BEZEL_TEST(object_instance_reclaimed_5437,
           using_var_object_instance_freed_spec_28_9) {
    JitResult r = runAotChecked(kUsingVarObject);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// §28.9 / §8.1 — the explicit-annotation plain `var` spelling.
BEZEL_TEST(object_instance_reclaimed_5437,
           plain_var_object_instance_freed_spec_28_9) {
    JitResult r = runAotChecked(kPlainVarObject);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// §28.9 / §8.1 — the inferred plain `var` spelling.
BEZEL_TEST(object_instance_reclaimed_5437,
           inferred_var_object_instance_freed_spec_28_9) {
    JitResult r = runAotChecked(kInferredVarObject);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// §8.6 / §28.9 — a BOX in an `Object`-typed slot keeps its OWN release rule.
// Green means the `Object` reclaim declined it (an unconditional reclaim would
// double-free the wrapper against the scope-end `bz_box_release`).
BEZEL_TEST(object_instance_reclaimed_5437,
           object_box_keeps_its_own_release_rule_8_6) {
    JitResult r = runAotChecked(kObjectSlotHoldsBox);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 86);
}

// §8.6 / §28.9 — a box written OVER an `Object` instance is not freed by the
// `Object` release. The runtime vtable-identity guard, on the exact shape a
// static slot-type test would get wrong.
BEZEL_TEST(object_instance_reclaimed_5437,
           object_slot_reassigned_to_box_keeps_box_8_6) {
    JitResult r = runAotChecked(kObjectSlotReassignedToBox);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 186);
}

// §28.9 — the CONTROL: `new Empty()`, an equally trivial-dispose user class,
// stays freed. Guards against the fix breaking ordinary trivial-dispose release.
BEZEL_TEST(object_instance_reclaimed_5437,
           empty_control_still_freed_spec_28_9) {
    JitResult r = runAotChecked(kEmptyControlOnly);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}