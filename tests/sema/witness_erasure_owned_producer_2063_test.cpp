// Spec-derived regression test for #2063 — §11.8/§28.1 substitution erasure
// on the witness-side conformance match (`Resolver::checkStaticInterfaceConformance`
// in src/sema/TypeResolution_Checks.cpp).
//
// §11.8 (v0.196, "Static interface members"): "a conforming type declares
// `class Meters: INumber<Meters>` and supplies the static members with exact
// signatures ... exact against the **substituted** signature (v0.196): at a
// value or runtime-managed conforming type a declared `owned` return or
// `out owned` slot is erased by the substitution (§28.1), so the witness is
// spelled without the marker (which would be a compile error on the concrete
// type)".
//
// §28.1 (v0.196, "Where the `owned` modifier can appear"): "for a slot whose
// declared type is a bare type parameter of the interface, the marker is read
// from the substituted signature, so at a value or runtime-managed implementing
// type a declared `owned` marker is erased by the substitution".
//
// Before #2063, the witness-side conformance match compared the PRE-
// substitution AST `returnOwned` bits directly, so a RECORD conformer to an
// owned-producer interface was wrongly REJECTED — the record-shaped witness
// cannot spell `owned` on its concrete type (§28.1 makes that an inadmissible
// position), so the post-substitution signature is the unmarked one. The
// #2048 sweep worked around the gap by retyping the lit
// `static_iface_conformance_807__record_conformance_accepted.bzl` fixture
// from `record Pt` to `class Pt`. This test exercises the underlying
// per-substitution erasure directly.
//
// The AOT run oracle is used: ACCEPT tests `ran && exitCode == expected`;
// REJECT tests `!ran && diagnostics.hasErrors()`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Compile-and-run via the AOT pipeline; returns true iff the program compiled,
// linked, and ran to normal termination with the expected exit status.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// Compile-and-run via the AOT pipeline; returns true iff the program was
// REJECTED at compile time with at least one error diagnostic.
bool rejects(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// ACCEPT case 1 (§11.8/§28.1, the #2063 gap): a RECORD conformer to an
// interface with an `owned T` static producer. The post-substitution
// signature erases `owned` (records are value types and carry no disposal
// obligation — §28.1), so the record's witness is spelled WITHOUT the
// marker. Pre-#2063 this was wrongly rejected.
const char* kRecordOwnedProducerAccepted =
    "module M; "
    "public interface IZ<T> where T: IZ<T> "
    "  static method Zero(): owned T; "
    "end IZ; "
    "public record Pt(x: int): IZ<Pt> "
    "  public static method Zero(): Pt => new Pt(0); "
    "end Pt; "
    "public class C "
    "  public static method Main(): int "
    "    var p := Pt.Zero(); "
    "    return p.x; "
    "  end Main; "
    "end C;";

// ACCEPT case 2 (non-erasure control): a CLASS conformer to the same
// interface. The class substitution RETAINS the marker (ordinary transfer
// meaning per §28.1), so the class's witness spells `owned Pt`. This case
// was already accepted pre-#2063 and must remain accepted.
const char* kClassOwnedProducerAccepted =
    "module M; "
    "public interface IZ<T> where T: IZ<T> "
    "  static method Zero(): owned T; "
    "end IZ; "
    "public class Pt: IZ<Pt> "
    "  public x: int; "
    "  public constructor(v: int) x := v; end; "
    "  public static method Zero(): owned Pt => new Pt(0); "
    "end Pt; "
    "public class C "
    "  public static method Main(): int "
    "    using var p := Pt.Zero(); "
    "    return p.x; "
    "  end Main; "
    "end C;";

// ACCEPT case 3 (parameter-slot erasure, §28.1 `out owned` arm): a RECORD
// conformer to an interface whose producer member has an `out owned T`
// parameter. The substituted slot is the unmarked `out` form, so the record
// witness is spelled `out` (the value-type out delivers a plain value).
const char* kRecordOutOwnedProducerAccepted =
    "module M; "
    "public interface IZ<T> where T: IZ<T> "
    "  static method TryZero(out owned value: T?): bool; "
    "end IZ; "
    "public record Pt(x: int): IZ<Pt> "
    "  public static method TryZero(out value: Pt?): bool "
    "    value := new Pt(7); return true; "
    "  end TryZero; "
    "end Pt; "
    "public class C "
    "  public static method Main(): int "
    "    if Pt.TryZero(out var v) then return v!.x; end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// REJECT control: a CLASS conformer that drops the `owned` marker from its
// witness. The class substitution retains the marker, so the unmarked
// witness is a real non-conformance (§11.2 owned-exact-match). Pre-#2063
// this was already rejected; the #2063 erasure must NOT widen acceptance
// to this case.
const char* kClassMissingOwnedRejected =
    "module M; "
    "public interface IZ<T> where T: IZ<T> "
    "  static method Zero(): owned T; "
    "end IZ; "
    "public class Pt: IZ<Pt> "
    "  public x: int; "
    "  public constructor(v: int) x := v; end; "
    "  public static method Zero(): Pt => new Pt(0); "
    "end Pt;";

}  // namespace

// ── §11.8/§28.1 (v0.196, #2063): the record-conformer erasure ───────────────

BEZEL_TEST(witness_erasure_owned_producer_2063,
           record_conformer_erases_owned_t_producer) {
    // per §11.8 (v0.196) + §28.1 (v0.196): a RECORD conformer to an owned-T
    // producer interface is ACCEPTED — `owned T` erases to plain `T` at the
    // value substitution, so the unmarked witness is the correct spelling.
    // Pre-#2063 this was rejected by the pre-substitution returnOwned compare.
    BEZEL_EXPECT_TRUE(runs(0, kRecordOwnedProducerAccepted));
}

BEZEL_TEST(witness_erasure_owned_producer_2063,
           class_conformer_retains_owned_t_producer) {
    // Non-erasure control: at a CLASS substitution the marker is retained
    // (§28.1), so the witness must spell `owned Pt`. Pre-#2063 and post, this
    // is accepted — the fix must not regress the class path.
    BEZEL_EXPECT_TRUE(runs(0, kClassOwnedProducerAccepted));
}

BEZEL_TEST(witness_erasure_owned_producer_2063,
           record_conformer_erases_out_owned_param_slot) {
    // per §28.1: at a value/runtime-managed substitution an `out owned T`
    // parameter slot erases to `out` (the value out delivers a plain value,
    // no disposal obligation). The record witness is spelled `out value: Pt?`.
    BEZEL_EXPECT_TRUE(runs(7, kRecordOutOwnedProducerAccepted));
}

BEZEL_TEST(witness_erasure_owned_producer_2063,
           class_conformer_missing_owned_still_rejected) {
    // Non-regression: a CLASS conformer dropping `owned` from its witness
    // remains a real non-conformance — the class substitution retains the
    // marker, so the unmarked witness fails the §11.2 exact-match rule. The
    // #2063 erasure must not over-accept this case.
    BEZEL_EXPECT_TRUE(rejects(kClassMissingOwnedRejected));
}
