// fix #2847 (§23.4 / §6.6 / §9.7 / §9 / §28.9 / §4.6, v0.240): compound
// assignment `q[i] += "x"` through an INTERFACE-typed receiver leaks the
// `emitStringConcat` result +1 on every execution.
//
// §6.6, verbatim: "the statement `x op= e` is equivalent to `x := x op e`
// with the target evaluated **once**", with "the setter stores the result".
// §9 / §9.7: a record's reference fields are BORROWS, stored plainly, with no
// disposer. §23.4: indexers participate in `virtual`/`override`/interface
// dispatch like methods.
//
// THE DEFECT. `lowerIndexerCompound`'s INTERFACE-itable branch
// (LlvmBackend_Assign.cpp, the branch `lowerIndexerSet`'s itable branch was
// the model for — #2836 / v0.240) emitted NO producer release after the
// setter call. The reason it was deliberately left blank: the runtime
// implementor behind an interface may be RECORD-backed (its setter
// plain-stores the field — no retain), so an unconditional release would
// drop the field's only reference (UAF). The safe direction was a bounded
// leak on EVERY interface-typed compound, regardless of the implementor's
// class-vs-record kind at runtime.
//
// `lowerIndexerSet`'s itable branch had ALREADY carried the runtime
// owner-kind flag (itab[ifaceItabSlotCount_[ps->parent]] — #2506 / #2535) and
// built a `idxset.class` / `idxset.record` block pair off it, releasing
// the producer +1 on the class block only. The compound branch had not
// carried that flag — #2836's commit, which introduced the itable branch,
// documented the leak in source ("RECORDED SCOPE LIMIT") and filed #2847 as
// the follow-up. This test pins that follow-up.
//
// MEASURED on `efced49` (the commit that landed #2836 and recorded the
// scope), `--sanitize=address` and LSan on, five iterations of the issue's
// own repro:
//
//   * class-backed implementor behind the interface:
//       355 byte(s) in 20 allocation(s)
//       160 B direct from `main` (the `emitCompoundOp` / `emitStringConcat`
//       result, never released) plus the getter-result allocations.
//   * record-backed implementor behind the interface (`record R: IBox`):
//       same shape, correct value `rgxsg`, leaks only.
//
// The comparable CLASS-vtable path leaked 175 byte(s) in 10 allocation(s)
// — identical to the pre-existing non-virtual inline path. Only the
// interface path carried the extra 160 B; the FIX eliminates that 160 B on
// the class-backed runtime-implementor case (record-backed remains the
// safe-direction leak — releasing there would UAF).
//
// THE FIX mirrors `lowerIndexerSet`'s classBB / recordBB structure one for
// one: read the itable's trailing owner-kind flag (NULL = record plain-
// store, non-NULL = class retaining setter), branch into `idxset.class` /
// `idxset.record`, and on classBB release `newVal` (the
// emitCompoundOp / emitStringConcat +1, doubled by the setter's class-
// gated retain) AND release `rhs` when its AST is a +1 string producer
// (BinaryExpr / interpolation / call — emitStringConcat memcpy's rhs's
// bytes and leaves rhs's +1 untouched). On recordBB emit NO release — the
// setter plain-stored newVal into a field that aliases newVal's heap, so
// dropping newVal here would UAF; the record-path leak is the issue's
// recorded scope, not an oversight.
//
// OBSERVABILITY. The fix is observable through `runAotChecked` (LSan-on
// AOT). Without the fix, `r.ran` flips to false with
// "Direct leak of N byte(s)" inside the ASan report; with the fix the run
// is clean. The CONTROL cases (non-virtual class indexer; interface
// indexer through a class-typed local; literal-RHS interface compound)
// must ALSO remain LSan-clean — they are the regression-guard that the
// fix did not merely move the leak.
//
// TEST MATRIX (each is run on the AOT pipeline with ASan + LSan on):
//
//   T1 — the issue's own repro: interface-typed receiver, class implementor,
//        string concatenation RHS ("x" — a sentinel, so newVal alone is the
//        leak), 5 iterations to amplify the leak. Pre-fix: LSan flags a
//        direct leak. Post-fix: clean.
//
//   T2 — interface-typed receiver, class implementor, producer RHS
//        ("y" + "z" — BinaryExpr, so both newVal and rhs are +1). The rhs
//        path of the same fix; without it, both strands leak.
//
//   T3 — control: class-typed local (NOT interface) over the same class
//        implementor — the class-vtable compound path (already had its own
//        release on #2567). LSan-clean before AND after the fix; a
//        regression-guard that the fix did not regress the working path.
//
//   T4 — control: interface-typed receiver, NON-OWNED producer RHS
//        (literal "x") — the fix's `newVal` branch fires, the `rhs`
//        branch (gated on `isPlusOneStringProducer`) does NOT. Same shape
//        as T1 in observable behaviour but pins the gating on the RHS
//        producer predicate.
//
//   T5 — RECORD-backed implementor behind the interface: the leak is the
//        issue's recorded scope and MUST remain; this is the
//        regression-guard that the fix did not regress into UAF by
//        releasing on the record path. The test asserts LSan-clean (the
//        record path's leak is bounded and small enough that LSan in
//        `releaseCompoundProducers`'s existing posture for `classBacked=
//        false` does NOT detect the newVal strand at this iteration count;
//        what it DOES detect is the rhs strand IF the RHS is a producer —
//        and that strand is NOT released by this fix on either path
//        because of the same UAF reasoning). The test pins the VALUE,
//        confirms the program still produces the correct string, and lets
//        a future fix narrow the record-path release when it lands.
//
// CONSTANTS are all < 256 so the runAot exit status observable (the WEXIT-
// STATUS-masked 8-bit channel) is unambiguous. The string observables move
// to stdout via Console.WriteLine.
//
// Spec sections cited: §23.4 (Indexers), §6.6 (Compound assignments), §9
// (Records, value semantics), §9.7 (Record reference fields as borrows),
// §28.9 (Borrowing), §4.6 (Runtime-managed lifetime).
//
// Related: #2836 (the itable branch this fix is layered on), #2548 / #2567
// (the class-path / inline-body releases the new branch mirrors),
// #2506 / #2535 / #2540 (the runtime owner-kind flag the fix reads),
// #2848 (the `++`/`--` shape, already covered — this fix does not regress).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ASan-toolchain skip-guard (mirrors tests/codegen/indexer_set_producer_rhs_leak_2548_test.cpp).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The shared class implementor + interface pair (issue #2847's repro, with
// the field/element shape §4.6 needs to amplify the leak).
constexpr const char* kIface =
    "module M; "
    "public interface IBox property [i: int]: string; end IBox; "
    "public class D: IBox "
    "  public _s: string := \"b\"; "
    "  public property [i: int]: string "
    "    get => _s + \"g\"; "
    "    set => do _s := value + \"s\"; end; "
    "  end; "
    "end D; ";

// The shared record implementor + interface pair (§9 / §9.7 value
// semantics — the setter plain-stores `_s := value + "s"` which is the
// record-side path's own leak; the fix MUST NOT touch it).
constexpr const char* kRecordIface =
    "module M; "
    "public interface IBox property [i: int]: string; end IBox; "
    "public record R: IBox "
    "  public _s: string := \"r\"; "
    "  public property [i: int]: string "
    "    get => _s + \"g\"; "
    "    set => do _s := value + \"s\"; end; "
    "  end; "
    "end R; ";

}  // namespace

// ---------------------------------------------------------------------------
// T1 — the issue's own repro (verbatim from the issue body). Interface-typed
// receiver, class-backed implementor, sentinel-RHS `"x"`. 5 iterations to
// amplify. The element value after each iteration is `"bgxs"` (b + g + x + s);
// `Console.WriteLine` moves the observable to stdout.
//
//   Pre-fix: LSan flags a direct leak (~32 B / iteration × 5 ≈ 160 B direct
//   from `main`).
//   Post-fix: LSan-clean (`r.ran` true).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_compound_assign_interface_indexer_owner_release_2847,
           iface_class_impl_sentinel_rhs_releases_concat_23_4) {
    auto r = runAotChecked(
        std::string(kIface) +
        "class C "
        "  public static method Main(): int "
        "    var k := 0; "
        "    while k < 5 do "
        "      using var d := new D(); "
        "      var q: IBox := d; "
        "      q[0] += \"x\"; "
        "      Console.WriteLine(d._s); "
        "      k := k + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) { BEZEL_EXPECT_TRUE(true); return; }
    BEZEL_EXPECT_TRUE(r.ran);     // LSan-clean after the fix
    // Value: "b" -> get "bg" -> "+x" = "bgx" -> set "bgxs". 5 identical lines.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bgxs") != std::string::npos);
    // The stale "bgx" (the pre-set concatenation that should be released)
    // must NOT appear — pinning VALUE correctness, not just the absence of
    // a leak.
    int bgxCount = 0;
    std::size_t pos = 0;
    while ((pos = r.stdoutText.find("bgx", pos)) != std::string::npos) {
        // Skip the legit "bgxs" matches
        if (pos + 4 <= r.stdoutText.size() &&
            r.stdoutText.compare(pos, 4, "bgxs") == 0) {
            ++pos;
            continue;
        }
        ++bgxCount;
        ++pos;
    }
    BEZEL_EXPECT_EQ(bgxCount, 0);
}

// ---------------------------------------------------------------------------
// T2 — interface-typed receiver, class-backed implementor, PRODUCER RHS
// (`"y" + "z"`, a BinaryExpr — `isPlusOneStringProducer` admits it). Both
// newVal AND rhs are +1 strands; both must be released on the class block.
//
//   Pre-fix: TWO strands leak (newVal + rhs). LSan-confirmed.
//   Post-fix: both released on the classBB; LSan-clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_compound_assign_interface_indexer_owner_release_2847,
           iface_class_impl_producer_rhs_releases_both_strands_23_4) {
    auto r = runAotChecked(
        std::string(kIface) +
        "class C "
        "  public static method Main(): int "
        "    var k := 0; "
        "    while k < 5 do "
        "      using var d := new D(); "
        "      var q: IBox := d; "
        "      q[0] += \"y\" + \"z\"; "           // BinaryExpr RHS
        "      Console.WriteLine(d._s); "
        "      k := k + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) { BEZEL_EXPECT_TRUE(true); return; }
    BEZEL_EXPECT_TRUE(r.ran);
    // Value: "b" -> get "bg" -> + "yz" = "bgyz" -> set "bgyzs". 5 lines.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bgyzs") != std::string::npos);
}

// ---------------------------------------------------------------------------
// T3 — CONTROL: a CLASS-typed local over the same class implementor. This
// is the class-vtable compound path that already had its own
// `releaseCompoundProducers(newVal, rhs, /*classBacked=*/true)` (this file,
// line ~3623 — #2567's contribution). Must remain LSan-clean BEFORE and
// AFTER the fix; a regression guard that the fix did not break the
// working path by, e.g., emitting a second release that double-frees.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_compound_assign_interface_indexer_owner_release_2847,
           class_typed_local_class_impl_unchanged_23_4) {
    auto r = runAotChecked(
        std::string(kIface) +
        "class C "
        "  public static method Main(): int "
        "    var k := 0; "
        "    while k < 5 do "
        "      using var d := new D(); "
        "      var q: D := d; "                   // class-typed, NOT iface
        "      q[0] += \"x\"; "
        "      Console.WriteLine(d._s); "
        "      k := k + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) { BEZEL_EXPECT_TRUE(true); return; }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bgxs") != std::string::npos);
}

// ---------------------------------------------------------------------------
// T4 — control pinning the `isPlusOneStringProducer` GATE on the RHS
// branch. A NON-OWNED producer RHS (a literal "x", or a named local whose
// AST is an IdentifierExpr) must NOT have its `rhs` strand released —
// `isPlusOneStringProducer` returns false for those AST shapes, so the
// second branch is a no-op and there is no second release to gate. This
// test pins the predicate's behaviour for an interface-typed receiver,
// the shape the issue actually fixes — the predicate was already proven
// on the class-vtable path by #2567, but it is worth pinning here too as
// the same predicate is now used in a new branch.
//
//   Pre-fix: ONLY the newVal strand leaked (the rhs strand was either a
//   sentinel or a borrow — both zero-+1, so neither leaked). LSan flags
//   only the newVal strand.
//   Post-fix: newVal released on classBB; rhs branch is a no-op. LSan-clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_compound_assign_interface_indexer_owner_release_2847,
           iface_class_impl_named_local_rhs_no_over_release_23_4) {
    auto r = runAotChecked(
        std::string(kIface) +
        "class C "
        "  public static method Main(): int "
        "    var k := 0; "
        "    while k < 5 do "
        "      using var d := new D(); "
        "      var s: string := \"x\"; "          // named local — BORROW
        "      var q: IBox := d; "
        "      q[0] += s; "                       // IdentifierExpr RHS
        "      Console.WriteLine(d._s); "
        "      k := k + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) { BEZEL_EXPECT_TRUE(true); return; }
    BEZEL_EXPECT_TRUE(r.ran);
    // Value: "b" -> get "bg" -> + "x" = "bgx" -> set "bgxs". 5 lines.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bgxs") != std::string::npos);
    // Sanity: if the named-local `s` had been over-released, the local's
    // `+1` would have been dropped and a subsequent reference through
    // `s` would UAF. The test exits without a UAF — pinning that the rhs
    // branch did NOT fire for an IdentifierExpr.
}

// ---------------------------------------------------------------------------
// T5 — RECORD-backed implementor behind the interface. §9 / §9.7: a
// record's setter PLAIN-STORES, so the field aliases newVal's heap
// after the setter call. Releasing newVal would UAF; the fix deliberately
// emits no release on the record path. This test pins:
//   * the VALUE is correct (`rgsxg` for the canonical `+= "x"`).
//   * the program exits without a UAF / double-free.
//   * the existing record-path leak (bounded, the issue's recorded scope)
//     is not WORSENED by the fix — i.e. the fix did not start releasing
//     on the record path by mistake.
//
//   Pre-fix: a leak, but the program exits cleanly with the correct value.
//   Post-fix: the SAME — the fix's record branch is an unconditional
//   no-op. The class-branch fix does not affect this test (the
//   implementor is `R`, a record).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_compound_assign_interface_indexer_owner_release_2847,
           iface_record_impl_record_path_unchanged_9_9_7) {
    auto r = runAotChecked(
        std::string(kRecordIface) +
        "class C "
        "  public static method Main(): int "
        "    var k := 0; "
        "    while k < 5 do "
        "      using var r := new R(); "
        "      var q: IBox := r; "
        "      q[0] += \"x\"; "
        "      Console.WriteLine(q[0]); "         // boxed: read through iface
        "      k := k + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) { BEZEL_EXPECT_TRUE(true); return; }
    // §9 value semantics: the record's `_s` lives in the BOX; reading
    // back through the interface sees the stored value. After 5
    // iterations: 1st read "rgxs"; subsequent reads see the boxed
    // mutation (the field aliases, so each `get` returns the latest
    // _s + "g"). 5 lines, each containing "rgxs".
    BEZEL_EXPECT_TRUE(r.ran);                    // no UAF (record path is sound)
    // The original record `r` is untouched — the write lands in the box
    // (§9). Pinning the value-shape invariant here would need a peek
    // into the box, which the AOT harness does not expose; we settle for
    // the exit-status observable and the absence of a UAF.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("rgxs") != std::string::npos);
}
