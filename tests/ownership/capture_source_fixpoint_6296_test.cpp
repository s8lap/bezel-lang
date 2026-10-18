// Spec-derived tests for issue #6296 — §28.3 (v0.338) *Provenance* clause 1's
// TRANSITIVE / LEAST-FIXPOINT requirement for a captured function value.
//
// §28.3 (v0.338) reads, for a lambda literal: it contributes "the sources of
// the borrows it captures … **a captured function value its own set,
// transitively**", and then — "Where these sets depend on one another through
// captures … they are **the least sets that satisfy this clause**: a cycle
// adds nothing beyond the sources its assignments carry in from outside it."
//
// Neither half was implemented. The capture-source walks classified each
// captured identifier only as an escape-bound owned local / catch binding, or
// as a borrow-bearing record/span/choice carrier — and a captured
// FUNCTION-typed local is none of those, so its own carried set was never read
// and the holder's set stayed EMPTY. The DIRECT twin of every shape below was
// therefore already rejected, correctly, with the §28.3/§9.7 diagnostic; only
// the one-hop-later shapes slipped through.
//
// The issue's own repro is the CYCLIC case, which additionally crashed codegen
// (Calls2 `disposeStack_.back()` on an empty stack) because sema admitted the
// program first. Note what that means for the test shape: a crash and a
// rejection both leave `r.ran == false`, so asserting only `!r.ran` would be a
// VACUOUS oracle here — every row below asserts the §28.3 diagnostic TEXT.
//
// ACCEPT rows are the load-bearing other half: the transitive rule must not
// over-reject. A carried source that is a PARAMETER outlives the call, and a
// closure that never escapes must stay legal; if the fixpoint were computed as
// an over-approximation, these would flip to rejections.
//
// per §28.3 (v0.338), §9.7

#include <string>

#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

// The §28.3 needle. Asserted as TEXT on every reject row because a crash and a
// rejection both report `r.ran == false` — the text is the only thing that
// distinguishes them.
static const char* kOutliveFragment = "lets the borrow outlive";

static const char* kItemClass =
    "module M; class Item "
    "  public constructor() end constructor; "
    "  public method N(): int => 3; "
    "end Item; ";

// ===========================================================================
// REJECT — the issue's repro: the CYCLIC capture-source shape.
// ===========================================================================

// §28.3 least-fixpoint clause, the issue's own case `s28_3_0147` verbatim in
// shape: two captured function-typed locals each assigned a lambda that calls
// the other, with an owned `using var` reached from inside the cycle. Per the
// clause the cycle "adds nothing beyond the sources its assignments carry in
// from outside it" — and what it carries in from outside is `it`, so `g`'s set
// is {it} and returning `g` lets that borrow outlive `it`.
//
// MEASURED before the fix: rc 139, a SIGSEGV in the compiler itself (sema
// admitted the program, then codegen crashed on the admitted program). MEASURED
// after: rejected with the §28.3/§9.7 outlive diagnostic.
// per §28.3 (v0.338) least-fixpoint clause, §28.3/§9.7 borrow-lifetime rule
BEZEL_TEST(capture_source_fixpoint_6296, cyclic_capture_source_rejected) {
    std::string src =
        std::string(kItemClass) +
        "class Maker "
        "  public static method Make(): () -> Item "
        "    var f: (() -> Item)? := null; "
        "    var g := lambda() => f!(); "
        "    using var it := new Item(); "
        "    f := lambda() => do var x := g(); return it; end; "
        "    return g; "
        "  end Make; "
        "end Maker;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOutliveFragment));
}

// §28.3 transitive clause, NO cycle: `g` captures `h`, `h` captures `it`.
//
// MEASURED before the fix: rc 0 — compiled CLEAN, a silent acceptance, with no
// cycle anywhere in the program. This row is why the defect is the missing
// transitive half rather than a cycle-handling bug, and it is the sharper
// regression pin: it fails on the same code path the cycle merely triggers.
// per §28.3 (v0.338) "a captured function value its own set, transitively"
BEZEL_TEST(capture_source_fixpoint_6296, transitive_two_hop_capture_source_rejected) {
    std::string src =
        std::string(kItemClass) +
        "class Maker "
        "  public static method Make(): () -> Item "
        "    using var it := new Item(); "
        "    var h := lambda() => do return it; end; "
        "    var g := lambda() => h(); "
        "    return g; "
        "  end Make; "
        "end Maker;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOutliveFragment));
}

// §28.3 transitive clause via a BARE-IDENTIFIER ALIAS: `g2 := g` must inherit
// what `g` carries, so returning the alias is the same escape as returning `g`.
// per §28.3 (v0.338) — a captured binding holds every value ever assigned to it
BEZEL_TEST(capture_source_fixpoint_6296, aliased_function_value_carries_source_rejected) {
    std::string src =
        std::string(kItemClass) +
        "class Maker "
        "  public static method Make(): () -> Item "
        "    using var it := new Item(); "
        "    var g := lambda() => do return it; end; "
        "    var g2 := g; "
        "    return g2; "
        "  end Make; "
        "end Maker;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOutliveFragment));
}

// §28.3 transitive clause on the RETURNED-LAMBDA leg: `return lambda() => h();`
// returns a closure that captures a function value carrying a borrow of an
// owned scope-bound local. A returned closure outlives the frame, so the
// captured borrow would dangle.
//
// MEASURED before the fix: rc 0 — compiled CLEAN.
// per §28.3 (v0.338) transitive clause, read on the escaping-closure path
BEZEL_TEST(capture_source_fixpoint_6296, returned_lambda_capturing_carrier_rejected) {
    std::string src =
        std::string(kItemClass) +
        "class Maker "
        "  public static method Make(): () -> Item "
        "    using var it := new Item(); "
        "    var h := lambda() => do return it; end; "
        "    return lambda() => h(); "
        "  end Make; "
        "end Maker;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOutliveFragment));
}

// ===========================================================================
// ACCEPT — the over-rejection controls. The fixpoint must not over-approximate.
// ===========================================================================

// §28.3 / §9.7: a carried source that is a PARAMETER outlives the call, so it is
// not an escape-bound obligation and a returned closure over it is legal — the
// caller re-establishes the source on its own side. Identical in shape to the
// two-hop reject row above, differing ONLY in the source being a parameter.
//
// NOTE on the spelling: the TWO-HOP chain (`h` captures `p`, `g` captures `h`)
// is deliberately NOT used here, on either the escaping or the non-escaping
// path. It leaks one 8-byte closure cell — a pre-existing codegen defect, filed
// as #4214 ("closure capturing a capturing closure leaks one 8-byte cell per
// instance"), whose trigger is precisely an inner closure capturing a
// PARAMETER. MEASURED on this branch WITH and WITHOUT the fix below, via
// `agent_check.sh --probe --asan`: "Direct leak of 8 byte(s) in 1 object(s)",
// frame `#1 bz$M.Maker$N`, on both the block- and expression-bodied spellings
// (see the comment on #4214 for the full measurement). Pinning that shape here
// would make this row fail for a reason that has nothing to do with §28.3, so
// the returned carrier is one hop.
//
// The two-hop ACCEPT surface is covered by the other two rows below instead:
// `source_free_carrier_chain_accepted` (two hops, nothing owned captured) and
// `locally_invoked_carrier_chain_accepted` (two hops, an owned `using var`
// source, carrier never escapes). Between those and this row the accept side
// covers: a parameter source escaping, a two-hop chain with no source, and a
// two-hop chain whose owned source outlives every use.
//
// MEASURED: rc 0 before and after the fix; this row is what would flip first if
// the leaf filter ever started treating a parameter as an escape-bound source.
// per §28.3, §9.7
BEZEL_TEST(capture_source_fixpoint_6296, param_sourced_carrier_returned_accepted) {
    std::string src =
        std::string(kItemClass) +
        "class Maker "
        "  public static method FromParam(p: Item): () -> Item "
        "    var h := lambda() => do return p; end; "
        "    return h; "
        "  end FromParam; "
        "  public static method Main(): int "
        "    using var it := new Item(); "
        "    var f := Maker.FromParam(it); "
        "    var got := f(); "
        "    return got.N(); "
        "  end Main; "
        "end Maker;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// §28.3: a carrier chain that captures NOTHING owned carries no source, so it
// must stay accepted. A fixpoint that seeded its leaves with every captured
// local rather than only escape-bound ones would reject this.
// per §28.3 (v0.338)
BEZEL_TEST(capture_source_fixpoint_6296, source_free_carrier_chain_accepted) {
    std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var h := lambda(): int => 1; "
        "    var g := lambda(): int => h(); "
        "    var f := g(); "
        "    return f + 40; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// §28.3: the carrier never ESCAPES — `g` is invoked before scope end, while `it`
// is still alive. The transitive rule constrains only a function value that
// outlives its source, so a locally-invoked chain must stay accepted and
// ASan-clean (this is the use-after-dispose the rejection exists to prevent, so
// the accept leg also proves the generated code is sound).
// per §28.3, §9.7
BEZEL_TEST(capture_source_fixpoint_6296, locally_invoked_carrier_chain_accepted) {
    std::string src =
        std::string(kItemClass) +
        "class C "
        "  public static method Main(): int "
        "    using var it := new Item(); "
        "    var h := lambda(): Item => it; "
        "    var g := lambda(): Item => h(); "
        "    var got := g(); "
        "    return got.N(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}