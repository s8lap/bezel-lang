// Regression tests for #7825 -- a method declared `: owned StringBuilder` could
// return a BORROW (`return this.sb`, `return arr[0]`, `return s ?? new ..`,
// `return s.Append("x")`) and the ownership pass accepted it; the same shapes on
// a user class were rejected. Since #7733 disposes an owned call-result chain
// root, the caller's `Bad(x).Length` then freed a builder owned elsewhere (a
// heap-use-after-free and a SEGV in bz_string_release).
//
// Language spec §28.3 (v0.350), announcement table row 1: "`owned T` ... Every
// return path supplies a fresh or owned value ...; a borrow is rejected." Row 4
// makes a field / indexer / parameter read and a `this`-returning call a borrow.
// §28.9: StringBuilder and Random are trivial-dispose CLASSES (stdlib §3.1,
// §2.7). §28.3 K7.3: a `??` meeting an owned arm and a borrowed arm is rejected.
//
// Root cause: ownedArgSourceIsBorrow (the owned-sink classifier checkOwned-
// ReturnDirectBorrow reuses) gated its field / indexer / property reads on
// isBorrowableType, which reads `declaration->kind`; a builtin class such as
// StringBuilder is a declaration-less Named TypeRef, so every non-parameter
// read of one was "not a borrow". A fluent call (`s.Append(..)`) is a CallExpr
// and was never classified at all.
//
// REJECT rows are runAot compile-time rejections; ACCEPT rows run on
// runAotChecked (ASan + LSan) and loop, since a one-shot leak repro is vacuous.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kBorrowReturn =
    "a borrow cannot be returned as an `owned` value";
constexpr const char* kMixedMeet =
    "mix an owned value with a borrowed arm";

const std::string kHead =
    "module M; import System; import System.Text; "
    "import System.Collections.Generic;\n";

void expectRejected(const std::string& body, const char* needle) {
    JitResult jr = runAot(kHead + body);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, needle));
}

void expectClean(const std::string& body, const char* out) {
    JitResult jr = runAotChecked(kHead + body);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
    BEZEL_EXPECT_EQ(jr.stdoutText, out);
}

}  // namespace

// REJECT: bad_owned_returns_borrow (issue repro).
BEZEL_TEST(owned_return_builtin_class_borrow_7825, field_read_rejected_spec_28_3) {
    expectRejected(R"BZL(
class Holder
  public sb: StringBuilder;
  public constructor(s: StringBuilder)
    this.sb := s;
  end constructor;
  public method Bad(): owned StringBuilder
    return this.sb;
  end Bad;
end Holder;
class C
  public static method Main(): int
    using var s := new StringBuilder("x");
    using var h := new Holder(s);
    Console.WriteLine(h.Bad().Length);
    return 0;
  end Main;
end C;
)BZL", kBorrowReturn);
}

// REJECT: bad5_V4.
BEZEL_TEST(owned_return_builtin_class_borrow_7825, fluent_param_append_rejected_spec_28_3) {
    expectRejected(R"BZL(
class C
  public static method Bad(s: StringBuilder): owned StringBuilder
    return s.Append("x");
  end Bad;
  public static method Main(): int
    using var s := new StringBuilder("x");
    Console.WriteLine(Bad(s).Length);
    return 0;
  end Main;
end C;
)BZL", kBorrowReturn);
}

// REJECT: bad5_V4, `=>` form.
BEZEL_TEST(owned_return_builtin_class_borrow_7825, fluent_param_append_exprbody_rejected_spec_28_3) {
    expectRejected(R"BZL(
class C
  public static method Bad(s: StringBuilder): owned StringBuilder => s.Append("x");
end C;
)BZL", kBorrowReturn);
}

// REJECT: this.sb.Append(..).Append(..).
BEZEL_TEST(owned_return_builtin_class_borrow_7825, fluent_chain_field_root_rejected_spec_28_3) {
    expectRejected(R"BZL(
class Holder
  public sb: StringBuilder;
  public constructor(s: StringBuilder)
    this.sb := s;
  end constructor;
  public method Bad(): owned StringBuilder
    return this.sb.Append("a").Append("b");
  end Bad;
end Holder;
)BZL", kBorrowReturn);
}

// REJECT: bad5_V5.
BEZEL_TEST(owned_return_builtin_class_borrow_7825, indexer_get_rejected_spec_28_3) {
    expectRejected(R"BZL(
class C
  public static method Bad(a: Array<StringBuilder>): owned StringBuilder
    return a[0];
  end Bad;
end C;
)BZL", kBorrowReturn);
}

// REJECT: bad5_V3.
BEZEL_TEST(owned_return_builtin_class_borrow_7825, coalesce_mixed_meet_rejected_spec_28_3) {
    expectRejected(R"BZL(
class C
  public static method Bad(s: StringBuilder?): owned StringBuilder
    return s ?? new StringBuilder("n");
  end Bad;
end C;
)BZL", kMixedMeet);
}

// REJECT: Random, the other trivial-dispose class (shared predicate).
BEZEL_TEST(owned_return_builtin_class_borrow_7825, random_field_rejected_spec_28_3) {
    expectRejected(R"BZL(
class Holder
  public r: Random;
  public constructor(r: Random)
    this.r := r;
  end constructor;
  public method Bad(): owned Random
    return this.r;
  end Bad;
end Holder;
)BZL", kBorrowReturn);
}

// REJECT: Array<Array<int>> element (shared predicate).
BEZEL_TEST(owned_return_builtin_class_borrow_7825, nested_array_indexer_rejected_spec_28_3) {
    expectRejected(R"BZL(
class C
  public static method Bad(a: Array<Array<int>>): owned Array<int>
    return a[0];
  end Bad;
end C;
)BZL", kBorrowReturn);
}

// REJECT: Dictionary value (shared predicate).
BEZEL_TEST(owned_return_builtin_class_borrow_7825, dictionary_value_indexer_rejected_spec_28_3) {
    expectRejected(R"BZL(
class C
  public static method Bad(d: Dictionary<int, Dictionary<int, int>>): owned Dictionary<int, int>
    return d[0];
  end Bad;
end C;
)BZL", kBorrowReturn);
}

// ACCEPT: return new StringBuilder(..).
BEZEL_TEST(owned_return_builtin_class_borrow_7825, fresh_new_accepted_spec_28_3) {
    expectClean(R"BZL(
class C
  public static method Mk(): owned StringBuilder
    return new StringBuilder("ab");
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Mk().Length;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL", "200\n");
}

// ACCEPT: return an owned local.
BEZEL_TEST(owned_return_builtin_class_borrow_7825, owned_local_accepted_spec_28_3) {
    expectClean(R"BZL(
class C
  public static method Mk(): owned StringBuilder
    var l := new StringBuilder("ab");
    l.Append("c");
    return l;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Mk().Length;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL", "300\n");
}

// ACCEPT: return Fresh() (an owned call).
BEZEL_TEST(owned_return_builtin_class_borrow_7825, owned_call_relay_accepted_spec_28_3) {
    expectClean(R"BZL(
class C
  public static method Fresh(): owned StringBuilder
    return new StringBuilder("ab");
  end Fresh;
  public static method Relay(): owned StringBuilder
    return Fresh();
  end Relay;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Relay().Length;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL", "200\n");
}

// ACCEPT: return Fresh().Append(..): the chain roots at a fresh owned producer.
BEZEL_TEST(owned_return_builtin_class_borrow_7825, fluent_on_owned_call_root_accepted_spec_28_3) {
    expectClean(R"BZL(
class C
  public static method Fresh(): owned StringBuilder
    return new StringBuilder("ab");
  end Fresh;
  public static method Mk(): owned StringBuilder
    return Fresh().Append("x");
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Mk().Length;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL", "300\n");
}

// ACCEPT: return new StringBuilder(..).Append(..).
BEZEL_TEST(owned_return_builtin_class_borrow_7825, fluent_on_new_root_accepted_spec_28_3) {
    expectClean(R"BZL(
class C
  public static method Mk(): owned StringBuilder
    return new StringBuilder("ab").Append("x");
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Mk().Length;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL", "300\n");
}

// ACCEPT: a plain (non-owned) StringBuilder return of a borrowed field is still a legal borrow.
BEZEL_TEST(owned_return_builtin_class_borrow_7825, borrow_return_plain_type_unchanged_accepted_spec_28_3) {
    expectClean(R"BZL(
class Holder
  public sb: StringBuilder;
  public constructor(s: StringBuilder)
    this.sb := s;
  end constructor;
  public method Get(): StringBuilder
    return this.sb;
  end Get;
  public method Fluent(): StringBuilder
    return this.sb.Append("y");
  end Fluent;
end Holder;
class C
  public static method Main(): int
    using var s := new StringBuilder("x");
    using var h := new Holder(s);
    var t := 0;
    for var i in 0..<100 do
      t += h.Get().Length - h.Get().Length;
    end for;
    Console.WriteLine(h.Fluent().Length);
    return 0;
  end Main;
end C;
)BZL", "2\n");
}

// ACCEPT: a range slice of an Array is a FRESH copy (§15.6), so returning one through `owned` is legal. Pins the slice exemption in the indexer arm; the twin of indexer_get_rejected.
BEZEL_TEST(owned_return_builtin_class_borrow_7825, range_slice_of_array_accepted_spec_15_6) {
    expectClean(R"BZL(
class C
  public static method Mk(a: Array<int>): owned Array<int>
    return a[0..1];
  end Mk;
  public static method Main(): int
    var xs: Array<int> := [7, 9, 11];
    var t := 0;
    for var i in 0..<100 do
      t += Mk(xs).Count;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL", "200\n");
}
