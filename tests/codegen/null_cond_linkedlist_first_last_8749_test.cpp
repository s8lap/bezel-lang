// Codegen -- spec section 7.5 / 7.7 (v0.359) over stdlib section 4.6 / 4.7:
// `l?.First` / `l?.Last` on a nullable `LinkedList<T>` / `OwningLinkedList<T>`
// receiver, issue 8749.
//
// stdlib section 4.6 declares `public pure readonly property First: T?` and
// `Last: T?` on `LinkedList<T>` (and section 4.7 on `OwningLinkedList<T>`).
// Spec section 7.5: "`x?.M` returns null if `x` is null; otherwise it performs
// the access. The result type is always nullable" -- and `T?` over a `T?`
// member flattens to `T?`. Spec section 7.7: `a ?? b` yields `b` when `a` is
// null, so `m?.First ?? -1` is the first element, or -1 for a null receiver
// AND for an empty list.
//
// THE DEFECT (MEASURED at the base, r30-c1 b279e8daf). sema left the
// `NullCondMemberExpr` untyped (the `.` form types the property in
// resolveMemberAccess's LinkedList node-property block; the `?.` twin did not
// exist) and codegen had no `?.` twin of the `.` form's `bz_llist_first_node`
// read, so the access lowered to NOTHING: the `Console.WriteLine` argument
// "failed to lower to a value, so the whole statement emits nothing -- not even
// the line terminator" (#3079), and `var x := m?.First` kept a zero slot (#2434).
// The program printed nothing and exited 0.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectOut(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// THE ISSUE'S REPRO, verbatim (the probe the issue was filed with). Expected
// 5 for the one-element list, then -1 for the null receiver.
BEZEL_TEST(null_cond_linkedlist_first_last_8749, issue_repro_verbatim) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Show(m: LinkedList<int>?)
    Console.WriteLine(m?.First ?? -1);
  end Show;
  public static method Main()
    using var l := new LinkedList<int>();
    var node := l.AddLast(5);
    Show(l);
    Show(null);
  end Main;
end Program;
)BZL"),
              "5\n-1\n");
}

// `?.Last` is the tail twin; a multi-element list tells head from tail, and an
// EMPTY list is null exactly as a null receiver is (stdlib section 4.6: the
// property is `T?`, null when empty).
BEZEL_TEST(null_cond_linkedlist_first_last_8749, last_head_tail_and_empty) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    using var e := new LinkedList<int>();
    var a := l.AddLast(4);
    var b := l.AddLast(6);
    Console.WriteLine(l?.First ?? -2);
    Console.WriteLine(l?.Last ?? -2);
    Console.WriteLine(e?.First ?? -2);
    Console.WriteLine(e?.Last ?? -2);
  end Main;
end Program;
)BZL"),
              "4\n6\n-2\n-2\n");
}

// The result is a real `int?` value: stored in a local it is the element for a
// present receiver and null for a null one (section 7.5), not a zero slot.
BEZEL_TEST(null_cond_linkedlist_first_last_8749, result_stored_in_nullable_local) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Read(m: LinkedList<int>?): int
    var x: int? := m?.First;
    return x ?? -3;
  end Read;
  public static method Main()
    using var l := new LinkedList<int>();
    var node := l.AddLast(7);
    Console.WriteLine(Read(l));
    Console.WriteLine(Read(null));
  end Main;
end Program;
)BZL"),
              "7\n-3\n");
}

// A reference element type: `T?` is a nullable pointer, not the value struct.
BEZEL_TEST(null_cond_linkedlist_first_last_8749, string_elements) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Tail(m: LinkedList<string>?)
    Console.WriteLine(m?.Last ?? "none");
  end Tail;
  public static method Main()
    using var l := new LinkedList<string>();
    var a := l.AddLast("x");
    var b := l.AddLast("z");
    Tail(l);
    Tail(null);
  end Main;
end Program;
)BZL"),
              "z\nnone\n");
}

// `SortedSet<T>.First` / `.Last` (stdlib section 4.18) are declared `T`, not
// `T?`; the `?.` spelling lifts them to `T?` (section 7.5), so a null receiver
// is null and a present one the least / greatest element. This is the
// SortedSet half of issue 6815, the same defect in the same arm.
BEZEL_TEST(null_cond_linkedlist_first_last_8749, sorted_set_first_last) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Both(m: SortedSet<int>?)
    Console.WriteLine(m?.First ?? -1);
    Console.WriteLine(m?.Last ?? -1);
  end Both;
  public static method Main()
    using var s := new SortedSet<int>();
    s.Add(8);
    s.Add(3);
    s.Add(5);
    Both(s);
    Both(null);
  end Main;
end Program;
)BZL"),
              "3\n8\n-1\n-1\n");
}
