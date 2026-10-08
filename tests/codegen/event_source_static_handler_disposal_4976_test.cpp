// fix #4976 — runtime: EventSource<T> field-disposal leaks handler closures
// (stdlib §6.5 v0.315 / language spec §24 / §27.5 / #841).
//
// §24 / §27.5 / #841: the source OWNS exactly one reference on every registered
// handler closure — codegen's bz_event_add / bz_event_subscribe hand it a
// transferred +1 the source must release exactly once. Before this fix
// bz_event_source_free only freed the handler array + struct, leaving every
// still-active handler closure leaked. With #4976 the disposal path iterates
// the handler list and releases each non-null closure; #4970's caller-side
// release for Remove (`-=`) handles the no-longer-subscribed closure at the
// call site, so the source's release covers only the still-active handlers.
//
// FAIL-WITHOUT proof: with the pre-#4976 bz_event_source_free (raw free of the
// array + struct, no closure iteration), the runAotChecked oracle reports a
// DIRECT leak of 2400 bytes (50 × 48 B per Add/+=, the { fn, env } 48-byte
// closure struct that the source still owned at each free time). With #4976
// the runAotChecked oracle reports no leak and exitCode == 0 — the spec-stated
// outcome for an Add-then-Raise-on-an-owned-source pattern.
//
// Spec authority:
//   - stdlib §6.5 (v0.315): `EventSource<T>` Subscribe / Add / Remove / Raise.
//   - language spec §24: "the source's destructor severs the subscriber list
//     before reclamation" (the §24 quiescence contract — and per §27.5 / #841
//     the subscriber's +1 reference is reclaimed on free).
//   - §27.5 / #841: a Bezel closure is a +1 reference, owned by the
//     registering source and released exactly once.
//   - §28.4 step 2: every owned field is disposed exactly once when its owner
//     is disposed — and the source's reclamation is responsible for its
//     dependents (the closure records).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// N-loop: the leak is per-add so a single Wire() leaks one 48 B record; looping
// the owner-construction N times makes the leak N× larger and harder to miss in
// the ASan summary. With #4976 all N records are released at the source's
// `bz_event_source_free` and LSan reports zero leaks.
const std::string kStaticHandlerNLoop =
    "module M;\n"
    "class H\n"
    "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On;\n"
    "end H;\n"
    "class Btn\n"
    "  private owned readonly _clicked: EventSource<int> := new();\n"
    "  public method Wire() _clicked.Add(H.On); end Wire;\n"
    "  public method Fire(n: int) _clicked.Raise(n); end Fire;\n"
    "end Btn;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var i := 0;\n"
    "    while i < 50 do\n"
    "      using var b := new Btn();\n"
    "      b.Wire();\n"
    "      b.Fire(i);\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

}  // namespace

// 50-iteration loop. The per-iteration leak (48 B) compounds so ASan reports
// 2400 B leaked; the run-flip is unambiguous. With #4976 all 50 records are
// released and LSan is clean.
BEZEL_TEST(codegen_event_source_static_handler_disposal_4976,
           owned_source_n_loop_releases_all_handlers_spec_27_5_28_4) {
    auto r = runAotChecked(kStaticHandlerNLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
