// Spec-derived regression test for issue #8960 — a call THROUGH A CLOSURE
// whose parameter is `owned`, passing a NAMED owned local WITHOUT the §22.3
// transfer marker, was ACCEPTED (and the program double-freed under ASan),
// while the method twin was rejected.
//
// §22.3 (v0.359) *Parameter modifiers*, the `owned` bullet: "**Call-site
// marker:** when the argument is a **named owned binding** — an owned local or
// an `owned` parameter, which the call leaves moved-from — the caller writes
// the transfer marker at the call site: `M(move x)` ... Omitting the marker on
// a named owned binding is a compile error naming the fix (\"this call consumes
// `x`; write `M(move x)`\")". The rule is a property of the ARGUMENT POSITION
// — a user `owned` formal — and not of the spelling of the call.
//
// §28.3.7 (v0.359) *Move semantics for owned locals*: "A callback that must
// genuinely consume a value takes an `owned` parameter and is invoked with the
// value transferred from outside (§28.9)."
//
// §28.9 (v0.359) *Runtime-managed types*: "a closure that must consume slot
// contents takes an owned-parameter argument and is invoked with `move`d slots
// from outside."
//
// THE DEFECT. The marker rule is enforced in the OWNERSHIP pass, which reads
// the callee's FORMAL LIST. A closure call's callee is a local of function
// type — it resolves to no MethodSymbol — so the formal list was empty, the
// closure's own `owned` parameter was never read as a sink, and the call site
// demanded no marker. The caller kept its disposal obligation while the
// closure disposed the value too: MEASURED on r42-m117-8960 @ r30-c1 —
// `f(a)` compiled with rc=0 and NO diagnostic and the run printed `1` then
// `ERROR: AddressSanitizer: attempting double-free`.
//
// The METHOD twin of the same shape (`c.Take(a)` over
// `method Take(owned ys: T)`) was rejected with §22.3's "this call consumes
// `a`; write `M(move a)`" — the asymmetry #8960 reports. The rows below pin
// the closure spelling to the method spelling.
//
// The two rows are the fix's own pins (both RED on the base): the bare
// identifier (the issue's literal repro) and its null-forgiving twin `a!`
// (§17.8's `!` is transparent to the marker, so the remedy is `M(move a!)` —
// #5144's peel). The controls that must keep their verdicts — the marked
// spelling, the fresh-producer spelling, the method twin, the §15.3
// move-only carve — are in the lane's probe directory under
// `corpus-candidates/`, not here: they are green on both arms, so a gtest row
// would pin nothing.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The issue's literal repro (#8960), reduced to the shape the ownership pass
// classifies: a closure whose parameter is `owned`, called with a NAMED owned
// local and no transfer marker.
//
// `call` is the argument spelling at the call site — "a" for the bare
// identifier, "a!" for its null-forgiving twin.
std::string closureCallWithUnmarkedOwnedLocal(const std::string& call) {
    return
        "module M; "
        "import System; "
        "public class C "
        "  public owned keep: Array<int>?; "
        "  public method Go(): int "
        "    var f := lambda(owned ys: Array<int>): int => do "
        "      this.keep := ys; "
        "      return 1; "
        "    end; "
        "    var a := new Array<int>(); "
        "    return f(" + call + "); "
        "  end Go; "
        "end C; ";
}

// True iff some ERROR diagnostic is §22.3's missing-marker diagnostic for the
// binding `name` — the exact text the METHOD twin produces, so the two
// spellings of one rule cannot drift apart.
bool saysCallConsumes(const OwnershipResult& r, const std::string& name) {
    return hasErrorMessage(r, "this call consumes `" + name + "`");
}

// True iff some ERROR diagnostic is §22.3's missing-marker diagnostic naming
// the `M(move X)` remedy for the binding `name`.
bool saysWriteMoveX(const OwnershipResult& r, const std::string& name) {
    return hasErrorMessage(r, "write `M(move " + name + ")`");
}

} // namespace

// ---------------------------------------------------------------------------
// §22.3 (v0.359) *Parameter modifiers* — the `owned` call-site marker is
// required at a closure's own `owned` parameter, exactly as at a user `owned`
// formal. RED on the base (accepted, and the program double-freed under
// ASan); GREEN with the fix.
// ---------------------------------------------------------------------------

BEZEL_TEST(closure_owned_param_missing_marker_8960,
           closure_call_with_unmarked_named_owned_local_is_rejected) {
    auto r = parseAndCheckOwnership(closureCallWithUnmarkedOwnedLocal("a"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The SPECIFIC rule: §22.3's missing-marker diagnostic, not an unrelated
    // error that happens to fire on the same program.
    BEZEL_EXPECT_TRUE(saysCallConsumes(r, "a"));
    BEZEL_EXPECT_TRUE(saysWriteMoveX(r, "a"));
}

// ---------------------------------------------------------------------------
// The §17.8 / #5144 twin: the argument is a null-forgiving `a!` over the
// named owned binding. The `!` is transparent to the marker, so the marker is
// still REQUIRED and the remedy keeps the `!` — `M(move a!)`. RED on the base
// (accepted); GREEN with the fix.
// ---------------------------------------------------------------------------

BEZEL_TEST(closure_owned_param_missing_marker_8960,
           closure_call_with_unmarked_named_owned_local_bang_is_rejected) {
    auto r = parseAndCheckOwnership(closureCallWithUnmarkedOwnedLocal("a!"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(saysCallConsumes(r, "a!"));
    BEZEL_EXPECT_TRUE(saysWriteMoveX(r, "a!"));
}
