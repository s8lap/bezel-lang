// Spec-derived ACCEPT controls for issue #2159 — a record with a borrow-bearing
// CHOICE field is a borrow-bearing record by §9.7 transitivity ("a borrow-
// bearing choice (§35.4) (transitivity)"). The REJECT side (return escape,
// instance-field store, async parameter) is pinned by the lit tests
// (tests/lit/ownership/record_choice_field_borrow_2159__*_rejected.bzl). This
// file pins the ACCEPT side: the new Choice leg of the record field walk must
// gate on classifyChoiceType == BorrowBearing, so (a) a record whose choice
// field is VALUE-LIKE stays value-like and freely returnable, and (b) a record
// carrying a genuinely borrow-bearing choice field is still legal for ordinary
// IN-FRAME use where the borrow's source outlives it — the over-reject failure
// mode. Both must run ASan/LSan-clean under the AOT oracle.
//
// per §9.7 (v0.226), §35.4, §28.3

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// §35.4.3: a record whose choice field is VALUE-LIKE (`VOpt` has only int/string
// payloads) is NOT borrow-bearing — it may be constructed, returned from a
// factory, and read back. The Choice leg must not widen to "any choice field".
// == 42.
BEZEL_TEST(record_choice_field_borrow_2159, value_like_choice_field_record_returned_accepted) {
    std::string src =
        "module M; "
        "choice VOpt Some(n: int), Named(s: string), None; end VOpt; "
        "record HolderV(K: VOpt); "
        "class C "
        "  public static method Make(): HolderV "
        "    return new HolderV(VOpt.Some(42)); "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := Make(); "
        "    var acc := 0; "
        "    case h.K of Some(var n) => do acc := n; end, "
        "                Named(var s) => do acc := s.Length; end, "
        "                None => do end end; "
        "    return acc; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §9.7 / §28.3: a record whose choice field carries a genuine SPAN borrow is
// borrow-bearing, but ordinary IN-FRAME use where the borrow's source (`local`)
// is declared first and outlives the record is legal — the §9.7 rules constrain
// ESCAPE, not in-frame use. Must stay accepted and ASan-clean (dangling never
// read). 7 + 11 == 18.
BEZEL_TEST(record_choice_field_borrow_2159, in_frame_span_choice_field_record_accepted) {
    std::string src =
        "module M; "
        "choice SpanC Some(s: ArraySpan<int>), None; end SpanC; "
        "record HolderC(K: SpanC); "
        "class C "
        "  public static method Main(): int "
        "    using var local := new Array<int>(); "
        "    local.Add(7); local.Add(11); "
        "    var h := new HolderC(SpanC.Some(local.AsSpan())); "
        "    var acc := 0; "
        "    case h.K of Some(var s) => do acc := s[0] + s[1]; end, None => do end end; "
        "    return acc; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 18);
}
