// Spec-derived tests for issue #7567 (and the parse leg of #7689) - the
// replacing `move` `move loc with e` (§28.8.1 *The replacing `move`*, Appendix
// A.7 `MoveExpr ::= "move" PrimaryExpr ("with" Expression)?`, v0.350).
//
// Before the fix the `with` after a move operand was consumed by the record
// update suffix (§9.5) and reported as the retired pre-v0.208 spelling, so every
// replacing `move` failed to PARSE: s28_8_1_0017 and every spec example
// (`return move _resource with new FileStream(...)`) died with "expected ';'
// after expression statement" plus the retired-spelling note.
//
// Scope of the fix: the clause is PARSED into the AST (UnaryExpr::replacement).
// The exchange itself - the receiver/index/replacement order, the one-step
// store-and-yield of the old value, the deleted restoration window - is the
// v0.336 tracker (issue #5923) and is NOT lowered: sema rejects a field or slot
// operand with a precise "not yet supported" diagnostic, and rejects the clause
// at a local binding or parameter as the spec requires (A.7 alternative (c): "a
// `with` clause is a compile error"). Both are asserted below, so a later lane
// that lowers the exchange must flip the field/slot rows deliberately.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Rejection must come from the sema diagnostic, never from a syntax error: the
// pre-fix diagnostics ("retired pre-v0.208 record-update spelling", "expected
// ';' after expression statement") must be absent.
void expectNoSyntaxError(const bezel::test::JitResult& r) {
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "retired pre-v0.208 record-update spelling"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "expected ';' after expression statement"));
}

const char* kHolder = R"BZL(
module M;

public class Res
  public constructor()
  end constructor;
end Res;

public class Holder
  public owned F: Res := new Res();
  public A: [2 owned]Res? := [new Res(), null];
  public constructor()
  end constructor;
end Holder;
)BZL";

}  // namespace

// §28.8.1 spec example `Replace` shape: `return move F with fresh;` at a field.
// Parses; sema declines the exchange precisely (issue #5923).
BEZEL_TEST(replacing_move_with_clause_7567,
           spec_28_8_1_return_move_field_with_parses_then_declined) {
    auto r = runAot(std::string(kHolder) + R"BZL(
public class User
  public static method Swap(h: Holder): owned Res
    return move h.F with new Res();
  end Swap;
end User;
)BZL");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    expectNoSyntaxError(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the replacing `move` (`move loc with e`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "#5923"));
}

// Issue #7689's shape: `using var x := move h.F with <expr>`.
BEZEL_TEST(replacing_move_with_clause_7567,
           spec_28_8_1_using_var_move_field_with_parses_then_declined) {
    auto r = runAot(std::string(kHolder) + R"BZL(
public class User
  public static method Run(h: Holder)
    using var old := move h.F with new Res();
  end Run;
end User;
)BZL");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    expectNoSyntaxError(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the replacing `move` (`move loc with e`"));
}

// §28.8.1: the collection-slot spelling `move coll[i] with e`.
BEZEL_TEST(replacing_move_with_clause_7567,
           spec_28_8_1_move_slot_with_parses_then_declined) {
    auto r = runAot(std::string(kHolder) + R"BZL(
public class User
  public static method Run(h: Holder)
    using var old := move h.A[0] with null;
  end Run;
end User;
)BZL");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    expectNoSyntaxError(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the replacing `move` (`move loc with e`"));
}

// A.7 MoveExpr alternative (c) / §28.8.1: "At (c) a `with` clause is a compile
// error" - the operand names a local binding. This is the sema rule itself (the
// conformance case s28_8_1_0023 pinned it only through the parse failure).
BEZEL_TEST(replacing_move_with_clause_7567,
           spec_28_8_1_with_clause_on_local_binding_rejected) {
    auto r = runAot(R"BZL(
module M;
public class Res
  public constructor()
  end constructor;
end Res;
public class Program
  public method Demo()
    var a := new Res();
    using var t := move a with new Res();
  end Demo;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    expectNoSyntaxError(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `with` clause is not admitted on the `move` of the local "
           "binding or parameter `a`"));
}

// ... and at an `owned` parameter, the other alternative-(c) operand.
BEZEL_TEST(replacing_move_with_clause_7567,
           spec_28_8_1_with_clause_on_owned_parameter_rejected) {
    auto r = runAot(R"BZL(
module M;
public class Res
  public constructor()
  end constructor;
end Res;
public class Program
  public static method Demo(owned p: Res)
    using var t := move p with new Res();
  end Demo;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    expectNoSyntaxError(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `with` clause is not admitted on the `move` of the local "
           "binding or parameter `p`"));
}
