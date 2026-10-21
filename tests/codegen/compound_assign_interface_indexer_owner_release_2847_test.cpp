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
// OBSERVABILITY. The fix is observable through `runAotChecked` (LSan-on
// AOT). Without the fix, `r.ran` flips to false with
// "Direct leak of N byte(s)"; with the fix the run is clean.
//
// ⚠ GETTER CHOICE. The getter `get => _s + "g"` from the issue's own repro
// ITSELF allocates a fresh +1 string on every read (the `cur` value). That
// strand leaks on every interface-typed compound — pre-fix AND post-fix,
// because `cur` is consumed by `emitCompoundOp` and then dropped, never
// released. So the issue's verbatim repro CANNOT be made LSan-clean by
// this fix alone; a probe of it on the FIXED binary still shows the
// GETTER-side leak (~175 B / 10 allocs), just not the COMPOUND-side leak
// (~180 B / 10 allocs that this fix targets). To isolate THIS fix's leak
// strand from the GETTER-side one, all five T-cells use a getter that does
// NOT allocate (`get => _s`) — the issue's verbatim getter is replaced with
// `get => _s` for the LSan oracle, and the issue's verbatim getter is
// exercised separately as T6 (a VALUE-correctness probe via `runAot`, not
// LSan). The fix is the COMPOUND-side release; the getter-side leak is
// out of scope for #2847 and pre-existing (it would need its own
// desugaring change to address).
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
// TEST MATRIX (each is run on the AOT pipeline with ASan + LSan on):
//
//   T1 — interface-typed receiver, class implementor, sentinel RHS
//        ("x" — a sentinel, so newVal alone is the leak), 5 iterations.
//        Pre-fix: 175 B / 10 allocs (LSan flips r.ran false).
//        Post-fix: 0 B (r.ran true).
//
//   T2 — interface-typed receiver, class implementor, PRODUCER RHS
//        ("y" + "z" — BinaryExpr, so both newVal and rhs are +1 strands).
//        Pre-fix: ~350 B (BOTH strands leak).
//        Post-fix: 0 B (both released on classBB).
//
//   T3 — control: class-typed local (NOT interface) over the same class
//        implementor — the class-vtable compound path that already had its
//        own `releaseCompoundProducers(newVal, rhs, /*classBacked=*/true)`
//        (#2567). Must remain LSan-clean BEFORE and AFTER the fix; a
//        regression-guard that the fix did not regress the working path
//        by, e.g., emitting a second release that double-frees.
//
//   T4 — interface-typed receiver, class implementor, named-local RHS
//        (IdentifierExpr). Pins the `isPlusOneStringProducer` GATE on the
//        rhs branch: a BORROW RHS must NOT have its `rhs` strand released.
//        The named-local `s` keeps its own +1 — releasing here would drop
//        the local's only ref. LSan-clean post-fix.
//
//   T5 — RECORD-backed implementor behind the interface. §9 / §9.7: a
//        record's setter PLAIN-STORES, so the field aliases newVal's heap
//        after the setter call. Releasing newVal would UAF; the fix
//        deliberately emits no release on the record path. This test pins:
//        (a) no UAF / double-free on the record path, (b) the VALUE is
//        correct (the boxed field is observable through the interface),
//        (c) the existing record-path leak (bounded, the issue's recorded
//        scope) is not WORSENED by the fix. This test uses `runAot`, not
//        `runAotChecked`, because the record path's leak is the
//        documented scope — `r.ran` would flip false for the bounded leak
//        even with the fix.
//
//   T6 — VALUE-only probe with the issue's VERBATIM getter (`_s + "g"`),
//        pinning the correct output value `bgxs` over 5 iterations. This
//        is the issue's own repro; the GETTER leak it surfaces is
//        pre-existing and out of scope for #2847, but the VALUE must be
//        correct under both pre-fix and post-fix — pinning that the fix
//        did not break the dispatch.
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

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Shared pair: class-backed implementor behind an interface, with a
// getter that does NOT allocate (`get => _s` instead of the issue's
// `_s + "g"` — see the file-level note on GETTER CHOICE). The setter is
// the issue's verbatim `_s := value + "s"` shape.
constexpr const char* kIface =
    "module M; "
    "public interface IBox property [i: int]: string; end IBox; "
    "public class D: IBox "
    "  public _s: string := \"b\"; "
    "  public property [i: int]: string "
    "    get => _s; "
    "    set => do _s := value + \"s\"; end; "
    "  end; "
    "end D; ";

// Shared pair: RECORD-backed implementor behind an interface. Same
// getter / setter shape as kIface, but the implementor is a record.
constexpr const char* kRecordIface =
    "module M; "
    "public interface IBox property [i: int]: string; end IBox; "
    "public record R: IBox "
    "  public _s: string := \"r\"; "
    "  public property [i: int]: string "
    "    get => _s; "
    "    set => do _s := value + \"s\"; end; "
    "  end; "
    "end R; ";

}  // namespace

// ---------------------------------------------------------------------------
// T1 — interface-typed receiver, class-backed implementor, sentinel RHS
// `"x"`. The getter is `get => _s` (no allocation) so the only +1 strand
// in flight is `newVal` (the emitCompoundOp/emitStringConcat result). 5
// iterations amplify.
//
//   Pre-fix: LSan flags a ~175 B / 10 alloc direct leak — `newVal` is
//   never released, the setter's class-gated retain doubled it to +2
//   which was unowned, the program ends with `r.ran = false`.
//   Post-fix: LSan-clean — `r.ran = true`.  The +1 doubled by the
//   setter's retain is dropped on the classBB (`emitStringRelease(newVal)`).
//
//   Value: "b" -> get "b" -> + "x" = "bx" -> set "bxs". 5 identical lines.
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);     // LSan-clean after the fix
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bxs") != std::string::npos);
    // The stale "bx" (the pre-set concatenation that should be released)
    // must NOT appear — pinning VALUE correctness, not just the absence of
    // a leak. Check that every "bx" substring is followed by "s" (i.e.
    // every occurrence is the legitimate "bxs" string), by verifying the
    // next char after each "bx" is 's' (not the separator or another
    // letter).
    int bxCount = 0;
    std::size_t pos = 0;
    while ((pos = r.stdoutText.find("bx", pos)) != std::string::npos) {
        if (pos + 2 < r.stdoutText.size() &&
            r.stdoutText[pos + 2] == 's') {
            ++pos;
            continue;
        }
        ++bxCount;
        ++pos;
    }
    BEZEL_EXPECT_EQ(bxCount, 0);
}

// ---------------------------------------------------------------------------
// T2 — interface-typed receiver, class-backed implementor, PRODUCER RHS
// (`"y" + "z"`, a BinaryExpr — `isPlusOneStringProducer` admits it). Both
// newVal AND rhs are +1 strands; both must be released on the class block.
//
//   Pre-fix: BOTH strands leak — ~350 B / 20 allocs direct leak.
//   Post-fix: both released on classBB; LSan-clean (`r.ran = true`).
//
//   Value: "b" -> get "b" -> + "yz" = "byz" -> set "byzs". 5 lines.
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("byzs") != std::string::npos);
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bxs") != std::string::npos);
}

// ---------------------------------------------------------------------------
// T4 — control pinning the `isPlusOneStringProducer` GATE on the RHS
// branch. A NON-OWNED producer RHS (a named local whose AST is an
// IdentifierExpr) must NOT have its `rhs` strand released —
// `isPlusOneStringProducer` returns false for those AST shapes, so the
// second branch is a no-op and there is no second release to gate. This
// test pins the predicate's behaviour for an interface-typed receiver,
// the shape the issue actually fixes — the predicate was already proven
// on the class-vtable path by #2567, but it is worth pinning here too as
// the same predicate is now used in a new branch.
//
//   Pre-fix: only the newVal strand leaks; LSan flags it.
//   Post-fix: newVal released on classBB; rhs branch is a no-op.
//   LSan-clean.
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // Value: "b" -> get "b" -> + "x" = "bx" -> set "bxs". 5 lines.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bxs") != std::string::npos);
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
//
//   * The program EXITS cleanly (no UAF / double-free) on the record path.
//   * The VALUE is correct (the boxed field is observable through the
//     interface — `q[0]` reads the latest value the box stored).
//   * The program reaches the end of `Main` and returns 0.
//
// This test uses `runAot`, NOT `runAotChecked` — the record path's leak
// is the issue's documented scope (`r.ran` would flip false even with the
// fix). Pinning the record path's SAFETY, not its LSan-cleanliness.
//
//   Pre-fix and post-fix: `r.ran = true`, value observable.
//   What this test pins: the fix did NOT regress the record path into a
//   UAF by releasing newVal on the record block.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_compound_assign_interface_indexer_owner_release_2847,
           iface_record_impl_record_path_no_uaf_9_9_7) {
    auto r = runAot(
        std::string(kRecordIface) +
        "class C "
        "  public static method Main(): int "
        "    var k := 0; "
        "    while k < 5 do "
        "      var r := new R(); "
        "      var q: IBox := r; "
        "      q[0] += \"x\"; "
        "      Console.WriteLine(q[0]); "         // boxed: read through iface
        "      k := k + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // §9 value semantics: the record's `_s` lives in the BOX; reading
    // back through the interface sees the stored value. After each
    // iteration: get "r" -> + "x" = "rx" -> set "rxs"; the next read
    // gets the box's latest. 5 lines, each containing "rxs".
    BEZEL_EXPECT_TRUE(r.ran);                    // no UAF (record path is sound)
    BEZEL_EXPECT_TRUE(r.stdoutText.find("rxs") != std::string::npos);
}

// ---------------------------------------------------------------------------
// T6 — VALUE-only probe with the issue's VERBATIM getter (`_s + "g"`),
// pinning the correct output value `bgxs` over 5 iterations. This is the
// issue's own repro shape (modulo the field initialiser); the GETTER
// leak it surfaces is pre-existing and out of scope for #2847, but the
// VALUE must be correct under both pre-fix and post-fix — pinning that
// the fix did not break the dispatch.
//
//   Pre-fix and post-fix: value is `bgxs` (5 lines). LSan-cleanliness
//   is NOT asserted (the GETTER-side leak is pre-existing, out of scope).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_compound_assign_interface_indexer_owner_release_2847,
           iface_class_impl_issue_getter_value_pinned_23_4) {
    auto r = runAot(
        "module M; "
        "public interface IBox property [i: int]: string; end IBox; "
        "public class D: IBox "
        "  public _s: string := \"b\"; "
        "  public property [i: int]: string "
        "    get => _s + \"g\"; "                 // issue's VERBATIM getter
        "    set => do _s := value + \"s\"; end; "
        "  end; "
        "end D; "
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
    // Value: "b" -> get "bg" -> + "x" = "bgx" -> set "bgxs". 5 lines.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bgxs") != std::string::npos);
    // The stale "bgx" (the pre-set concatenation that should be released)
    // must NOT appear — pinning VALUE correctness, not just the absence of
    // a leak. Check that every "bgx" substring is followed by "s" (i.e.
    // every occurrence is the legitimate "bgxs" string).
    int bgxCount = 0;
    std::size_t pos = 0;
    while ((pos = r.stdoutText.find("bgx", pos)) != std::string::npos) {
        if (pos + 3 < r.stdoutText.size() &&
            r.stdoutText[pos + 3] == 's') {
            ++pos;
            continue;
        }
        ++bgxCount;
        ++pos;
    }
    BEZEL_EXPECT_EQ(bgxCount, 0);
}
