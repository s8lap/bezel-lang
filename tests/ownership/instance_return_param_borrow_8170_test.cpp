// Spec-derived tests for issue #8170 -- an INSTANCE method returning its plain
// (borrowed) builtin-collection parameter (`Array<T>`, `Dictionary<K,V>`,
// `LinkedList<T>`) through a non-`owned` return (nullable or not, block- or
// expression-bodied) is rejected exactly like the user-class twin.
//
// Spec §28.3 (v0.355), the borrow-lifetime rule, K8.4: an instance method's
// non-`owned` return is attributed by every caller to the RECEIVER, so the only
// borrow it may return is receiver-sourced ("a receiver-sourced borrow from an
// instance method (a bare owned-field read, `this`, or a borrow reached through
// the receiver's fields and indexers)"); "a borrowed local or parameter with no
// source that outlives the call ... may not be returned".
//
// The defect: checkInstanceBorrowReturn gated on isBorrowableType, which reads
// `declaration->kind` and so answered false for a declaration-less builtin
// class such as `Array<T>`; every spelling over such a parameter slipped
// through (a user class `Item` was rejected in every spelling). The gate now
// also recognises the borrow-bearing builtin class heads
// (isBorrowableOwnedSinkType).
//
// per §28.3, K8.4, v0.355

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& members) {
    return "module M; class Holder "
           "  public owned _arr: Array<string>; "
           "  public constructor() _arr := new Array<string>(2); "
           "    _arr.Add(\"a\"); _arr.Add(\"b\"); end constructor; " +
           members +
           " end Holder; "
           "class Program public static method Main(): int " +
           "var h := new Holder(); Console.WriteLine(h.Fine().Count); "
           "return 0; end Main; end Program;";
}

constexpr const char* kFine =
    "public method Fine(): Array<string> => _arr; ";

void expectRejected(const std::string& members) {
    JitResult r = runAot(prog(members + kFine));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a borrow reached through parameter `a` may not be returned from "
           "an instance method"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§28.3"));
}

}  // namespace

// ---- must-reject: the issue's two spellings -------------------------------

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_nullable_block_array_rejected) {
    expectRejected("public method IdN(a: Array<string>?): Array<string>? "
                   "return a; end IdN; ");
}

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_expression_body_array_rejected) {
    expectRejected("public method IdE(a: Array<string>): Array<string> => a; ");
}

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_nullable_expression_body_array_rejected) {
    expectRejected("public method IdNE(a: Array<string>?): Array<string>? => a; ");
}

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_block_array_rejected) {
    expectRejected("public method IdB(a: Array<string>): Array<string> "
                   "return a; end IdB; ");
}

// ---- must-reject: other builtin collection classes ------------------------

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_dictionary_rejected) {
    expectRejected("public method IdD(a: Dictionary<string, int>): "
                   "Dictionary<string, int> return a; end IdD; ");
}

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_nullable_dictionary_expression_rejected) {
    expectRejected("public method IdDN(a: Dictionary<string, int>?): "
                   "Dictionary<string, int>? => a; ");
}

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_linkedlist_expression_rejected) {
    expectRejected("public method IdL(a: LinkedList<string>): "
                   "LinkedList<string> => a; ");
}

// ---- admitted controls (checked oracle, exact stdout) ---------------------

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_receiver_sourced_field_return_admitted) {
    // A bare owned-field read is receiver-sourced: admitted, nullable and not,
    // block- and expression-bodied.
    JitResult r = runAotChecked(
        "module M; class Holder "
        "  public owned _arr: Array<string>; "
        "  public constructor() _arr := new Array<string>(2); "
        "    _arr.Add(\"a\"); _arr.Add(\"b\"); end constructor; "
        "  public method E(a: Array<string>): Array<string> => _arr; "
        "  public method B(a: Array<string>): Array<string> "
        "    return _arr; end B; "
        "  public method N(a: Array<string>?): Array<string>? => _arr; "
        "end Holder; "
        "class Program public static method Main(): int "
        "  var h := new Holder(); "
        "  var p := new Array<string>(1); "
        "  Console.WriteLine(h.E(p)[1]); "
        "  Console.WriteLine(h.B(p)[0]); "
        "  Console.WriteLine(h.N(p)!.Count); "
        "  return 0; end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("b\na\n2\n"));
}

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_owned_param_and_owned_return_admitted) {
    // An `owned` return transfers ownership (any source); an `owned` parameter
    // is not a plain borrow.
    JitResult r = runAotChecked(
        "module M; class Holder "
        "  public constructor() end constructor; "
        "  public method Take(owned a: Array<string>): owned Array<string> "
        "    return a; end Take; "
        "  public method Fresh(): owned Array<string> => "
        "    new Array<string>(3); "
        "end Holder; "
        "class Program public static method Main(): int "
        "  var h := new Holder(); "
        "  var t := new Array<string>(4); t.Add(\"q\"); t.Add(\"r\"); "
        "  using var x := h.Take(move t); "
        "  using var y := h.Fresh(); "
        "  Console.WriteLine(x.Count + y.Count); "
        "  return 0; end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(instance_return_param_borrow_8170, spec_28_3_this_return_admitted) {
    JitResult r = runAotChecked(
        "module M; class Holder "
        "  public V: int := 5; "
        "  public constructor() end constructor; "
        "  public method Me(a: Array<string>): Holder => this; "
        "end Holder; "
        "class Program public static method Main(): int "
        "  var h := new Holder(); "
        "  var p := new Array<string>(1); "
        "  Console.WriteLine(h.Me(p).V); "
        "  return 0; end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}
