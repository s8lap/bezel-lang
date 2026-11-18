// Codegen -- spec section 7.5 / 7.7 (v0.359) over stdlib section 4.6 / 4.7:
// `l?.FirstNode` / `l?.LastNode` on a nullable `LinkedList<T>` /
// `OwningLinkedList<T>` receiver, issue 8770.
//
// stdlib section 4.6 declares `public pure readonly property FirstNode:
// LinkedListNode<T>?` and `public pure readonly property LastNode:
// LinkedListNode<T>?` on `LinkedList<T>` (section 4.7 the same pair on
// `OwningLinkedList<T>`), and on `LinkedListNode<T>`: `public readonly property
// Value: T`, `Next: LinkedListNode<T>?` and `Previous: LinkedListNode<T>?`.
// Spec section 7.5: "`x?.M` returns null if `x` is null; otherwise it performs
// the access. The result type is always nullable" -- the node properties are
// ALREADY nullable, so the `?.` result is the member's own type
// `LinkedListNode<T>?` (a nullable REFERENCE, not a nullable value), and a
// segment whose own declared type is nullable needs its own `?.` before the
// next suffix -- which is why the chain is `l?.FirstNode?.Value`. Spec section
// 7.7: `a ?? b` yields `b` when `a` is null.
//
// THE DEFECT (MEASURED at the base, r30-c1 f045631b1). sema left the
// `NullCondMemberExpr` untyped (the `.` form types the property in
// resolveMemberAccess's LinkedList node-property block; the `?.` twin did not
// exist) and codegen had no `?.` twin of the `.` form's `bz_llist_first_node`
// read, so the access lowered to NOTHING: the `Console.WriteLine` argument
// "failed to lower to a value, so the whole statement emits nothing -- not
// even the line terminator" (#3079), and `var n := m?.FirstNode` kept a zero
// slot (#2434). The issue's repro printed `False` / `False` and nothing else.
//
// The SECOND segment (`n?.Value` on a `LinkedListNode<T>` receiver) had the
// same gap and is reachable through the `.` form alone: `var n := l.FirstNode;
// n?.Value` printed nothing at the base, while the section 7.4 diagnostic on
// the `.` spelling of the same member NAMES `n?.Value` as the remedy.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

void expectOut(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// THE ISSUE'S REPRO, verbatim (the probe the issue was filed with). The node
// is present for the one-element list (so `n = null` is False and the node's
// value is 1) and absent for the null receiver (so `n = null` is True and
// `?? -1` yields -1).
BEZEL_TEST(null_cond_linkedlist_first_last_node_8770, issue_repro_verbatim) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method C(m: LinkedList<int>?)
    var n := m?.FirstNode;
    Console.WriteLine(n = null);
    Console.WriteLine(m?.FirstNode?.Value ?? -1);
  end C;
  public static method Main()
    using var l := new LinkedList<int>();
    var a := l.AddLast(1);
    C(l);
    C(null);
  end Main;
end Program;
)BZL"),
              "False\n1\nTrue\n-1\n");
}

// `?.LastNode` is the tail twin, and the node is a NODE: the head node's
// value and the tail node's value differ on a two-element list.
BEZEL_TEST(null_cond_linkedlist_first_last_node_8770, head_and_tail_node) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Both(m: LinkedList<int>?)
    Console.WriteLine(m?.FirstNode?.Value ?? -1);
    Console.WriteLine(m?.LastNode?.Value ?? -2);
  end Both;
  public static method Main()
    using var l := new LinkedList<int>();
    var a := l.AddLast(4);
    var b := l.AddLast(6);
    Both(l);
    Both(null);
  end Main;
end Program;
)BZL"),
              "4\n6\n-1\n-2\n");
}

// An EMPTY but non-null list: the section 4.6 node property is
// `LinkedListNode<T>?` -- null when the list is empty -- so the `?.` access
// PERFORMS the access and the result is a null node, exactly as the `.` form
// gives (the node pointer the getter returns is null).
BEZEL_TEST(null_cond_linkedlist_first_last_node_8770, empty_list_is_a_null_node) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    using var e := new LinkedList<int>();
    var n := e?.FirstNode;
    Console.WriteLine(n = null);
    Console.WriteLine(e?.FirstNode?.Value ?? -1);
    Console.WriteLine(e?.LastNode?.Value ?? -2);
  end Main;
end Program;
)BZL"),
              "True\n-1\n-2\n");
}

// The node's value is a real `int?` value: stored in a local it is the
// element for a present node and null for an absent one (section 7.5), not a
// zero slot.
BEZEL_TEST(null_cond_linkedlist_first_last_node_8770, node_value_in_a_nullable_local) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Read(m: LinkedList<int>?): int
    var v: int? := m?.FirstNode?.Value;
    return v ?? -3;
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

// A reference element type: the node's `Value: T?` is a nullable POINTER, not
// the tagged value carrier, and the `??` fallback is the reference's own.
BEZEL_TEST(null_cond_linkedlist_first_last_node_8770, string_elements) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Tail(m: LinkedList<string>?)
    Console.WriteLine(m?.LastNode?.Value ?? "none");
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

// stdlib section 4.7: `OwningLinkedList<T>` declares the same node
// properties, so the `?.` spelling types and lowers the same way.
BEZEL_TEST(null_cond_linkedlist_first_last_node_8770, owning_linked_list) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Item
  public V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  destructor end destructor;
end Item;
public class Program
  public static method Head(m: OwningLinkedList<Item>?)
    Console.WriteLine(m?.FirstNode?.Value?.V ?? -1);
  end Head;
  public static method Main()
    using var o := new OwningLinkedList<Item>();
    _ := o.AddLast(move (new Item(9)));
    Head(o);
    Head(null);
  end Main;
end Program;
)BZL"),
              "9\n-1\n");
}

// The node's own accessor family (`Value`, `Next`, `Previous` -- stdlib
// section 4.6) under the `?.` spelling: a null node yields null, a present
// one the next / previous node.
BEZEL_TEST(null_cond_linkedlist_first_last_node_8770, node_next_and_previous) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    var a := l.AddLast(1);
    var b := l.AddLast(2);
    var n := l.FirstNode;
    Console.WriteLine(n?.Next?.Value ?? -1);
    Console.WriteLine(n?.Previous?.Value ?? -2);
    Console.WriteLine(l.LastNode?.Next = null);
  end Main;
end Program;
)BZL"),
              "2\n-2\nTrue\n");
}
