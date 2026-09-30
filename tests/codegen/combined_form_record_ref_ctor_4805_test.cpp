// fix #4805 (§22.3, §9.3, v0.307): combined-form record's explicit constructor
// with a `ref` parameter, called with a plain literal argument (not `ref`-bound),
// must emit the clean §22.3 call-site ref/out mismatch diagnostic — not the
// internal "lowerExpr returned null for call argument in buildOrderedArgs" ICE.
//
// Row 5 also pins the indexer/property fallback path (#1302's
// `emitAccessorRefArgCopyIn`): the §22.3 diagnostic block must run AFTER the
// fallback, so an indexer-slot ref argument still bracket-copies correctly.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Row 1: ICE repro — combined-form record with ref-param, literal arg → clean §22.3
BEZEL_TEST(rec_ref_ctor_4805, combined_form_literal_arg_rejects_clean_22_3) {
    // The exact repro from #4805: combined-form record with explicit ctor
    // `constructor(ref x: int)`, called with literal `6` instead of `ref v`.
    // Must reject with the clean §22.3 diagnostic, NOT the #1386 ICE.
    const char src[] = R"(
module M;
public record RefMod4775(X: int)
  public constructor(ref x: int)
    X := x;
  end constructor;
end RefMod4775;
public class Program
  public static method Main(): int
    var v := new RefMod4775(6);
    return v.X;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The diagnostic MUST include the clean §22.3 text, NOT the ICE.
    bool hasMismatch = false;
    bool hasParamRef = false;
    bool hasSection = false;
    bool hasIce = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("call-site `ref`/`out` mismatch") != std::string::npos)
            hasMismatch = true;
        if (d.message.find("parameter `x` is `ref`") != std::string::npos)
            hasParamRef = true;
        if (d.message.find("§22.3") != std::string::npos)
            hasSection = true;
        if (d.message.find("internal: lowerExpr returned null for call argument in buildOrderedArgs") != std::string::npos)
            hasIce = true;
    }
    BEZEL_EXPECT_TRUE(hasMismatch);
    BEZEL_EXPECT_TRUE(hasParamRef);
    BEZEL_EXPECT_TRUE(hasSection);
    BEZEL_EXPECT_FALSE(hasIce);
}

// Row 2: accept case — combined-form record with ref-param, caller uses `ref v`
BEZEL_TEST(rec_ref_ctor_4805, combined_form_ref_bound_arg_accepts_and_runs) {
    const char src[] = R"(
module M;
public record RefModAcc(X: int)
  public constructor(ref x: int)
    X := x;
  end constructor;
end RefModAcc;
public class Program
  public static method Main(): int
    var v := 42;
    var r := new RefModAcc(ref v);
    return r.X;
  end Main;
end Program;
)";
    auto r = runAot(src);
    // In Bezel, `return X` sets the process exit code; stdout is empty unless
    // the program explicitly calls Console.WriteLine.  Constructor binds
    // `X := x` where `ref x` aliases `v := 42`, so `r.X` is 42 and the
    // process returns 42.
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// Row 3: regression guard — body-only record with ref-param, literal arg → still clean §22.3
BEZEL_TEST(rec_ref_ctor_4805, body_only_ref_ctor_literal_arg_still_rejects_clean_22_3) {
    // Body-only record (no combined-form syntax), same literal-arg shape.
    // Must still produce the clean §22.3 diagnostic — no regression.
    const char src[] = R"(
module M;
public record RefModBodyOnly
  public X: int;
  public constructor(ref x: int)
    X := x;
  end constructor;
end RefModBodyOnly;
public class Program
  public static method Main(): int
    var v := new RefModBodyOnly(6);
    return v.X;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool hasMismatch = false;
    bool hasSection = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("call-site `ref`/`out` mismatch") != std::string::npos)
            hasMismatch = true;
        if (d.message.find("§22.3") != std::string::npos)
            hasSection = true;
    }
    BEZEL_EXPECT_TRUE(hasMismatch);
    BEZEL_EXPECT_TRUE(hasSection);
}

// Row 4: regression guard — combined-form record with NON-ref param, literal arg → ACCEPT
BEZEL_TEST(rec_ref_ctor_4805, combined_form_non_ref_param_literal_arg_accepts) {
    // Combined-form record whose explicit ctor has NO ref-param; literal arg
    // must be accepted and the program must run correctly — no spurious
    // ref-mismatch diagnostic.
    const char src[] = R"(
module M;
public record NonRefRec(Y: int)
  public constructor(y: long)
    Y := int(y);
  end constructor;
end NonRefRec;
public class Program
  public static method Main(): int
    var r := new NonRefRec(99L);
    return r.Y;
  end Main;
end Program;
)";
    auto r = runAot(src);
    // In Bezel, `return X` sets the process exit code; stdout is empty unless
    // the program explicitly calls Console.WriteLine.  Constructor binds
    // `Y := y` where `y := 99` (literal arg), so `r.Y` is 99 and the
    // process returns 99.
    BEZEL_EXPECT_EQ(r.exitCode, 99);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// Row 5: regression pin — indexer-slot ref argument through a method call
// must still bracket-copy (#1302's copy-in / copy-out) and NOT be wrongly
// rejected with the §22.3 diagnostic. The producer's wrong-place #4805
// patch short-circuited `emitAccessorRefArgCopyIn` by returning `false`
// before the fallback ran, breaking three rows in
// `tests/codegen/nullcond_ref_out_2318_test.cpp` (file header lists
// `ref_to_indexer_slot_writes_back_on_nonnull_receiver`,
// `ref_to_indexer_slot_index_not_evaluated_and_setter_not_called_on_null_receiver`,
// `ref_to_indexer_slot_index_evaluated_once_on_nonnull_receiver`). This
// row pins the simplest non-null-conditional indexer-slot shape so a
// future refactor of the §22.3 diagnostic block cannot reintroduce the
// regression.
BEZEL_TEST(combined_form_record_ref_ctor_4805, indexer_slot_ref_arg_accepts_and_runs) {
    const char src[] = R"(
module M;
public class Helper
  public static method Bump(ref a: int) a := a + 5; end Bump;
end Helper;
public class Program
  public static method Main(): int
    var arr := new Array<int>();
    arr.Add(10);
    Helper.Bump(ref arr[0]);
    return arr[0];
  end Main;
end Program;
)";
    auto r = runAot(src);
    // §22.3 bracket-copy (#1302): `arr[0]` starts at 10, `Bump` writes
    // 10 + 5 = 15 into the slot, the write-back is flushed by the caller
    // after normal return.
    BEZEL_EXPECT_EQ(r.exitCode, 15);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

}  // namespace
