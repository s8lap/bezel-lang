// #5823 (spec §3.6 *Nullable forms*, §30.14 nullable bullet, v0.338):
// a nullable form `V?` is read as `V` for the sendable and borrow-bearing
// predicates, whether written or manufactured by substitution.
//
// Probe-first result (MEASURED on the base before this test was added): the
// nullable-form reading already holds at these sites, so this file pins the
// existing behavior and adds no compiler change.
//
// Rows:
//   R1  Channel<Pair?> where Pair is a borrow-bearing record (§3.6: Pair? holds
//       exactly what Pair holds; §30.14: a borrow-bearing record is not
//       sendable) is rejected at the `new Channel<T>()` type argument.
//   R2  Channel<Widget?> for an open class Widget (§30.14 rule 1, Widget? is
//       sendable exactly where Widget is) is rejected.
//   R3  Gen.Id<Pair?>(new Pair(it)) returned from a method past the scope-bound
//       local `it` is rejected (§3.6 nullable forms through the bare-type-
//       parameter clause of §28.3 / §9.7 return rule).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── R1 — Channel<Pair?> over a borrow-bearing record is rejected as not sendable.
BEZEL_TEST(sema_nullable_form_borrow_5823,
           channel_borrow_bearing_nullable_element_rejected_spec_3_6_and_30_14) {
    auto r = runAot(
        "module M; import System.Threading; "
        "public class Item public constructor() end constructor; end Item; "
        "public record Pair(Item: Item); "
        "public class C public static method Main() "
        "  using var cr := new Channel<Pair?>(); "
        "  Console.WriteLine(\"ok\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not sendable"));
}

// ── R2 — Channel<Widget?> over an open class is rejected (Widget? sendable iff Widget).
BEZEL_TEST(sema_nullable_form_borrow_5823,
           channel_open_class_nullable_element_rejected_spec_30_14) {
    auto r = runAot(
        "module M; import System.Threading; "
        "public class Widget public constructor() end constructor; end Widget; "
        "public class C public static method Main() "
        "  using var cr := new Channel<Widget?>(); "
        "  Console.WriteLine(\"ok\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not sendable"));
}

// ── R3 — returning Id<Pair?>(new Pair(it)) past local `it` is rejected.
BEZEL_TEST(sema_nullable_form_borrow_5823,
           generic_id_nullable_borrow_return_past_local_rejected_spec_3_6) {
    auto r = runAot(
        "module M; "
        "public class Item public constructor() end constructor; end Item; "
        "public record Pair(Item: Item); "
        "public class Gen public static method Id<T>(a: T): T => a; end Gen; "
        "public class Client "
        "  public static method Make(): Pair? "
        "    var it := new Item(); "
        "    return Gen.Id<Pair?>(new Pair(it)); "
        "  end Make; "
        "end Client; "
        "public class Program public static method Main() "
        "  var p := Client.Make(); "
        "  Console.WriteLine(\"ok\"); "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "returned borrow-bearing record carries a borrow"));
}
