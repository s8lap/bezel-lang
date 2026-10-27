// Codegen — a NAMED (non-virtual) property declared `: Object` on a PUBLIC
// class emitted malformed IR and the program could not be built at all.
// Issue #2864.
//
// Spec (revision v0.240):
//   §8.6 "Boxing" — "When a value type is used where `Object` (or an interface
//     implemented by the value type) is expected, the value is **boxed**: a
//     heap-allocated wrapper holds the value. Boxed values are runtime-managed
//     ([§28.9])."
//   §23.2 "Expression-bodied read-only properties" — the `=> expr` spelling.
//   §23.3 "Block-form properties" — the `get => do … end;` spelling, which has
//     the same defect through the same emitter.
//   §17.4 "The `is`, `is not`, and `as` operators" — what the caller does with
//     the box, and the payload oracle used below.
//   §2.5 "Dependency resolution" — why the accessor exists at all: "the
//     compiler resolves each dependency by reading the imported module's
//     compiled artifact, which embeds the module's interface ([§2.8.2])", so a
//     PUBLIC property's accessor is part of that artifact and
//     `emitExportedPropertyGetters` emits a standalone `T get(ptr this)` for it
//     whether or not anything imports this module.
//
// THE BUG (measured on bd209a2). `public property Q: Object => 5;` on a public
// class emitted
//
//     Function return type does not match operand type of return inst!
//       ret i32 5
//      ptr
//     bezelc: codegen error: IR verification failed
//
// — a HARD BUILD FAILURE whose diagnostic points at IR, not at source. The
// §8.6 box was never emitted: `ensurePropertyAccessorFn` (LlvmBackend_Assign
// .cpp) does call `boxForTarget` on the body result, but `boxValue` resolves
// the box vtable through `primBoxVtable_`/`boxVtable_`, which
// `setupBoxDescriptors()` fills — and that pass used to run AFTER the four
// `emitExported{Property,Indexer}{Getters,Setters}` emitters that request the
// accessor body. `boxVtableFor` returned null, `boxForTarget` was a silent
// no-op, and the raw `i32` reached the `ptr` return. The fix moves
// `setupBoxDescriptors()` to immediately after `declareVtables()`.
//
// WHAT IS NOT FIXED, deliberately: the VIRTUAL/vtable face
// (`public virtual property O: Object => 0;`), whose accessor body is lowered
// by `declareVtables()` Pass D — before `setupBoxDescriptors` can possibly run,
// since it depends on Pass C's filled descriptors. That is #2759, still pinned
// as unbuildable by `expr_getter_object_boxing_2761_test.cpp`. Moving the pass
// one step further (between Pass C and Pass D) was TRIED and REGRESSES a
// working record program to a SIGSEGV — see the call-site note in
// LlvmBackend_impl.hpp.
//
// ⚠ ORACLE CHOICE. These cases use the PLAIN `runAot` value oracle, not
// `runAotChecked`: the §8.6 box a property read mints is reclaimed only once
// #2865's fix is also present, and a test must not depend on a sibling commit
// the gate may drop. The leak oracles live in
// box_temp_release_and_interp_2865_test.cpp; the assertions here are about the
// build succeeding and the VALUE being right.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// The issue's own repro, shaped to answer 41 rather than 1 so a silently-false
// test is distinguishable from a silently-zero exit. Pre-fix this program did
// not BUILD.
const char* kNamedObjectProperty =
    "module M2864A; "
    "public class P2864A "
    "  public property Q: Object => 5; "
    "end P2864A; "
    "public class C2864A "
    "  public static method Main(): int "
    "    using var p := new P2864A(); "
    "    if p.Q is int then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2864A;";

// §17.4 — the PAYLOAD, not just the type tag. A box whose descriptor matched
// but whose bytes were wrong would return 36.
const char* kNamedObjectPropertyUnbox =
    "module M2864B; "
    "public class P2864B "
    "  public property Q: Object => 5; "
    "end P2864B; "
    "public class C2864B "
    "  public static method Main(): int "
    "    using var p := new P2864B(); "
    "    var o := p.Q; "
    "    var r: int? := o as int; "
    "    return (r ?? 0) + 36; "   // 5 + 36 = 41
    "  end Main; "
    "end C2864B;";

// §23.3 — the BLOCK spelling of the identical property. A different arm of
// ensurePropertyAccessorFn (lowerScopedBlock -> lowerReturn) reached through
// the SAME exported-accessor emitter, and it failed identically.
const char* kNamedObjectBlockProperty =
    "module M2864C; "
    "public class P2864C "
    "  public property Q: Object "
    "    get => do "
    "      return 5; "
    "    end; "
    "  end Q; "
    "end P2864C; "
    "public class C2864C "
    "  public static method Main(): int "
    "    using var p := new P2864C(); "
    "    var o := p.Q; "
    "    var r: int? := o as int; "
    "    return (r ?? 0) + 36; "
    "  end Main; "
    "end C2864C;";

// The BOXABLE-KIND grid on one public class, so the fix is not pinned to `int`.
// `decimal` boxes only since #2793 and a RECORD/ENUM only through the
// descriptor machinery #2796 widened, so this doubles as a check that the
// reorder did not disturb either. `string` and a CLASS instance are the
// NON-boxing controls: §8.6 boxes a VALUE type, and a reference reaches Object
// as itself — if the reorder had made them box, `is` would walk a box
// descriptor instead of their own and go false.
const char* kNamedObjectPropertyKindGrid =
    "module M2864D; "
    "public record Pt2864D(X: int, Y: int); "
    "public enum Color2864D Red, Green; end Color2864D; "
    "public class Inner2864D public v: int; end Inner2864D; "
    "public class G2864D "
    "  private _s: string; "
    "  private owned _i: Inner2864D; "
    "  public constructor(s: string) _s := s; _i := new Inner2864D(); end constructor; "
    "  public property Qi: Object => 5; "
    "  public property Qd: Object => 2.5; "
    "  public property Qc: Object => 'z'; "
    "  public property Qb: Object => true; "
    "  public property Qm: Object => 12.5m; "
    "  public property Qr: Object => new Pt2864D(3, 4); "
    "  public property Qe: Object => Color2864D.Green; "
    "  public property Qs: Object => _s; "
    "  public property Qk: Object => _i; "
    "end G2864D; "
    "public class C2864D "
    "  public static method Main(): int "
    "    var hits: int := 0; "
    "    using var g := new G2864D(\"ab\" + \"cd\"); "
    "    if g.Qi is int     then hits := hits + 1; end; "
    "    if g.Qd is double  then hits := hits + 1; end; "
    "    if g.Qc is char    then hits := hits + 1; end; "
    "    if g.Qb is bool    then hits := hits + 1; end; "
    "    if g.Qm is decimal then hits := hits + 1; end; "
    "    if g.Qr is Pt2864D then hits := hits + 1; end; "
    "    if g.Qe is Color2864D then hits := hits + 1; end; "
    "    if g.Qs is string  then hits := hits + 1; end; "        // control
    "    if g.Qk is Inner2864D then hits := hits + 1; end; "     // control
    "    if hits = 9 then return 41; end; "
    "    return hits; "
    "  end Main; "
    "end C2864D;";

// OVER-CORRECTION GUARD, green BEFORE and AFTER. A non-Object declared type is
// untouched by the pass reorder, and nothing may now be minted for it: the
// checked run pins the absence of a wrapper across 20 reads.
const char* kNonObjectPropertyUnchanged =
    "module M2864E; "
    "public class P2864E "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property N: int => _n * 3; "
    "  public property M: int? => _n; "
    "end P2864E; "
    "public class C2864E "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    while i < 20 do "
    "      using var p := new P2864E(); "
    "      t := t + p.N + (p.M ?? 0); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length; "
    "      i := i + 1; "
    "    end while; "
    "    if t = 560 then return 41; end; "   // 20 * (21 + 7)
    "    return 1; "
    "  end Main; "
    "end C2864E;";

// A PRIVATE property on a non-public class was ALWAYS buildable — no exported
// accessor is emitted for it, so `ensurePropertyAccessorFn` is never requested
// and the inert `boxForTarget` was never reached. Pinned so the difference
// between the two spellings stays visible: this is what localized the defect to
// the exported-accessor face rather than to the read site.
const char* kPrivatePropertyControl =
    "module M2864F; "
    "class P2864F "
    "  private property Q: Object => 5; "
    "  public method Probe(): int "
    "    if Q is int then return 41; end; "
    "    return 1; "
    "  end Probe; "
    "end P2864F; "
    "public class C2864F "
    "  public static method Main(): int "
    "    using var p := new P2864F(); "
    "    return p.Probe(); "
    "  end Main; "
    "end C2864F;";

}  // namespace

// ===========================================================================
// (A) The issue's repro. // per §8.6, §23.2, §2.5
// ===========================================================================
BEZEL_TEST(codegen_named_object_property_boxing_2864,
           named_object_property_builds_and_boxes_spec_8_6) {
    JitResult r = runAot(kNamedObjectProperty);
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: false — IR verification rejected it
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §17.4 — the unboxed PAYLOAD, so a box with the right tag and the wrong
// bytes still fails.
BEZEL_TEST(codegen_named_object_property_boxing_2864,
           named_object_property_round_trips_payload_spec_17_4) {
    JitResult r = runAot(kNamedObjectPropertyUnbox);
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: false
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §23.3 — the block spelling through the same emitter.
BEZEL_TEST(codegen_named_object_property_boxing_2864,
           named_object_block_property_builds_spec_23_3) {
    JitResult r = runAot(kNamedObjectBlockProperty);
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: false
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6 — every boxable kind, plus the two non-boxing controls.
BEZEL_TEST(codegen_named_object_property_boxing_2864,
           named_object_property_kind_grid_spec_8_6) {
    JitResult r = runAot(kNamedObjectPropertyKindGrid);
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: false
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (B) Over-correction guards — green BEFORE and AFTER the fix.
// ===========================================================================
// per §23.2, §7 — a non-Object declared type mints nothing.
BEZEL_TEST(codegen_named_object_property_boxing_2864,
           non_object_named_property_unchanged_spec_23_2) {
    JitResult plain = runAot(kNonObjectPropertyUnchanged);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kNonObjectPropertyUnchanged);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // LSan forces 1 on any leak
}

// per §2.5, §13 "Visibility" — the non-exported spelling, which never failed.
BEZEL_TEST(codegen_named_object_property_boxing_2864,
           private_object_property_control_spec_2_5) {
    JitResult r = runAot(kPrivatePropertyControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}
