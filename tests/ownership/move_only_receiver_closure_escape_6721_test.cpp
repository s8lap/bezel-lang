// Regression tests for #6721's ownership legs: a closure over a value kind's
// `this` that escapes, judged by what the capture contributes.
//
// Spec (v0.340), §9.4 *The receiver value is fixed at the call*:
//   "In a move-only receiver's member body (below) a lambda's capture is
//    §28.9's move-only capture — a borrow of the in-place storage, bounded by
//    the call — and such a closure may not outlive the activation"
//   "the sources of the borrows the value carries where it holds them ..., so
//    the closure meets the sink rule as any capture-bearing one does"
// §28.3 sink rule: "While its source set is non-empty, a function value may
// not be stored in a field, in any collection, in a property or indexer, or
// through an `out` or `ref` parameter, may not be returned from the enclosing
// method".
//
// An owning-classified choice (§35.4) is move-only (§15.3). Before the fix its
// escaping `this` closure was accepted and read the receiver's dead frame
// (ASan stack-use-after-return on the frame-escape row below).
//
// per §9.4, §15.3, §28.3, §28.9, §35.4 (v0.340)

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

const char* kRes6721 =
    "module M; "
    "public class Res "
    "  public N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "  destructor end destructor; "
    "end Res; ";

constexpr const char* kMoveOnlyReason =
    "that is move-only (an owning-classified choice, §35.4), the closure's "
    "capture of `this` is a borrow of the receiver's in-place storage, "
    "bounded by the call (§9.4, §28.9)";
}  // namespace

// The closure is returned from the member: it would outlive the activation.
BEZEL_TEST(move_only_receiver_closure_escape_6721,
           owning_choice_this_closure_returned_rejected_spec_9_4) {
    auto r = runAot(std::string(kRes6721) +
        "public choice Own "
        "  A(owned r: Res), "
        "  B; "
        "  public method Count(): int => (case this of A(r) => r.N, B => -1); "
        "  public method Maker(): () -> int "
        "    return lambda(): int => Count(); "
        "  end Maker; "
        "end Own; "
        "public class Program "
        "  public static method Main() "
        "    using var o := Own.A(new Res(5)); "
        "    var f := o.Maker(); "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "closure returned from the method"));
    BEZEL_EXPECT_TRUE(diagContains(r, kMoveOnlyReason));
}

// The reviewer's frame-escape shape: the receiver is a local of a helper whose
// frame is gone when the closure runs (ASan stack-use-after-return before the
// fix). Explicit `this` spelling.
BEZEL_TEST(move_only_receiver_closure_escape_6721,
           owning_choice_this_closure_escapes_frame_rejected_spec_9_4) {
    auto r = runAot(std::string(kRes6721) +
        "public choice Own "
        "  A(owned r: Res), "
        "  B; "
        "  public method Count(): int => (case this of A(r) => r.N, B => -1); "
        "  public method Maker(): () -> int "
        "    return lambda(): int => this.Count(); "
        "  end Maker; "
        "end Own; "
        "public class Program "
        "  private static method MakeFrom(): () -> int "
        "    using var o := Own.A(new Res(5)); "
        "    return o.Maker(); "
        "  end MakeFrom; "
        "  public static method Main() "
        "    var f := MakeFrom(); "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kMoveOnlyReason));
}

// "in any collection": the same closure stored into a collection parameter.
BEZEL_TEST(move_only_receiver_closure_escape_6721,
           owning_choice_this_closure_collection_store_rejected_spec_9_4) {
    auto r = runAot(std::string(kRes6721) +
        "public choice Own "
        "  A(owned r: Res), "
        "  B; "
        "  public method Count(): int => (case this of A(r) => r.N, B => -1); "
        "  public method Register(fs: Array<() -> int>) "
        "    fs.Add(lambda(): int => Count()); "
        "  end Register; "
        "end Own; "
        "public class Program "
        "  public static method Main() "
        "    using var o := Own.A(new Res(5)); "
        "    var fs := new Array<() -> int>(); "
        "    o.Register(fs); "
        "    Console.WriteLine(fs.Count); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "stored in a collection"));
    BEZEL_EXPECT_TRUE(diagContains(r, kMoveOnlyReason));
}

// Over-rejection guard: the same closure bound to a local and invoked within
// the body does not outlive the activation, and is admitted.
BEZEL_TEST(move_only_receiver_closure_escape_6721,
           owning_choice_this_closure_invoked_in_body_accepted_spec_9_4) {
    auto r = runAotChecked(std::string(kRes6721) +
        "public choice Own "
        "  A(owned r: Res), "
        "  B; "
        "  public method Count(): int => (case this of A(r) => r.N, B => -1); "
        "  public method Twice(): int "
        "    var f := lambda(): int => this.Count() * 2; "
        "    return f(); "
        "  end Twice; "
        "end Own; "
        "public class Program "
        "  public static method Main() "
        "    using var o := Own.A(new Res(5)); "
        "    Console.WriteLine(o.Twice()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// A record holding a class-typed member carries a borrow (§9.7: "Reference-
// typed fields in records are always borrows"), so the copy carries it and the
// returned closure meets the sink rule.
BEZEL_TEST(move_only_receiver_closure_escape_6721,
           borrow_bearing_record_this_closure_returned_rejected_spec_9_4) {
    auto r = runAot(
        "module M; "
        "public class Item "
        "  public Name: string; "
        "  public constructor(n: string) Name := n; end constructor; "
        "end Item; "
        "public record View(Item: Item) "
        "  public method Namer(): () -> string "
        "    return lambda(): string => this.Item.Name; "
        "  end Namer; "
        "end View; "
        "public class Program "
        "  public static method Main() "
        "    var it := new Item(\"x\"); "
        "    Console.WriteLine(new View(it).Namer()()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "closure returned from the method"));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "the copy carries the borrows that value holds (§9.4, §9.7)"));
}

// Over-rejection guard: "bound, invoked and passed within the body under that
// bullet" — the same closure invoked in the member body is admitted.
BEZEL_TEST(move_only_receiver_closure_escape_6721,
           borrow_bearing_record_this_closure_invoked_in_body_accepted_spec_9_4) {
    auto r = runAotChecked(
        "module M; "
        "public class Item "
        "  public Name: string; "
        "  public constructor(n: string) Name := n; end constructor; "
        "end Item; "
        "public record View(Item: Item) "
        "  public method Name(): string "
        "    var f := lambda(): string => this.Item.Name; "
        "    return f(); "
        "  end Name; "
        "end View; "
        "public class Program "
        "  public static method Main() "
        "    var it := new Item(\"x\"); "
        "    Console.WriteLine(new View(it).Name()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x\n"));
}
