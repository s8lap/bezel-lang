// #8392 / §28.3.10 (v0.357) — the must-use lint's premise is an owned value
// that reaches its scope's end UNDISCHARGED.
//
// §28.3.10 *Owned-local discharge requirement* states the lint's predicate
// and its CLOSED set of suppressions:
//
//   "When an owned local — or an `owned` parameter — reaches a *normal-exit*
//    scope end without having reached a discharge point, and is not in
//    moved-from state, it is disposed implicitly at that scope end …"
//
//   "The warning is **suppressed** by exactly these conditions …
//    3. it is moved or returned (no implicit drop occurs);"
//
// A `return cur;` IS a discharge: the value reaches the RETURN, not the
// scope end, so no implicit drop occurs there and the lint must not fire.
//
// The false positive this file pins has a specific shape: the local is
// REASSIGNED inside a LOOP and returned afterwards. The dataflow is right
// about it — the local reaches the function exit Disposed (returned on every
// path) — but the lint fired from the ownership pass's syntactic
// nested-scope fallback, whose "the slot was definitely restored" marker is a
// non-monotone, visit-order-dependent set: the loop's back edge makes the
// traversal visit the loop BODY after the block that discharges the local, so
// the body's unconditional reassignment re-inserted the local AFTER the
// discharge removed it.
//
// The rows below are the issue's own repro (the literal one, and the
// destructor-bearing twin the issue measured as well), plus the TWIN that
// STILL warns: the same loop-reassignment shape whose local is NOT returned
// is an undischarged owner, and the lint must keep firing for it.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

// The issue's own repro, verbatim apart from the formatting: a plain-`var`
// owned local, reassigned in a loop, returned on every path. `Node` has no
// destructor (it is a trivial-dispose class), so §28.9's ceremony exemption
// is not what silences it — the §28.3.10 suppression 3 is.
BEZEL_TEST(ownership_28_3_8392,
           local_returned_after_loop_does_not_draw_must_use_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module P;\n"
        "public class Node\n"
        "end Node;\n"
        "public class Program\n"
        "  private static method Build(n: int): owned Node\n"
        "    var cur := new Node();\n"
        "    for var i in 1..n do\n"
        "      cur := new Node();\n"
        "    end;\n"
        "    return cur;\n"
        "  end Build;\n"
        "  public static method Main()\n"
        "    using var x := Build(3);\n"
        "    Console.WriteLine(\"ok\");\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "cur"));
}

// The issue's own measurement, repeated: "a `Node` with a destructor draws
// the same warning in the loop form". With a destructor the local is NOT
// trivial-dispose, so §28.9's ceremony exemption does not apply and the row
// above's quietness cannot be explained by it — only by §28.3.10's
// suppression 3 ("it is moved or returned (no implicit drop occurs)").
BEZEL_TEST(ownership_28_3_8392,
           local_returned_after_loop_with_destructor_does_not_draw_must_use_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module P;\n"
        "public class Node\n"
        "  destructor\n"
        "    Console.WriteLine(\"bye\");\n"
        "  end destructor;\n"
        "end Node;\n"
        "public class Program\n"
        "  private static method Build(n: int): owned Node\n"
        "    var cur := new Node();\n"
        "    for var i in 1..n do\n"
        "      cur := new Node();\n"
        "    end;\n"
        "    return cur;\n"
        "  end Build;\n"
        "  public static method Main()\n"
        "    using var x := Build(3);\n"
        "    Console.WriteLine(\"ok\");\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "cur"));
}

// The TWIN of the rows above, and the control the fix must not weaken: the
// SAME loop-reassignment shape whose local is NOT returned is an
// undischarged owner — it reaches the scope end still owned — so the lint
// MUST fire, naming `cur`. Without this row the quiet rows above would be
// satisfied by the lint never firing for a reassigned local, which is the
// vacuous-green shape.
BEZEL_TEST(ownership_28_3_8392,
           local_not_returned_after_loop_still_draws_must_use_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module P;\n"
        "public class Node\n"
        "  destructor\n"
        "    Console.WriteLine(\"bye\");\n"
        "  end destructor;\n"
        "end Node;\n"
        "public class Program\n"
        "  private static method Leak(n: int)\n"
        "    var cur := new Node();\n"
        "    for var i in 1..n do\n"
        "      cur := new Node();\n"
        "    end;\n"
        "  end Leak;\n"
        "  public static method Main()\n"
        "    Leak(3);\n"
        "    Console.WriteLine(\"ok\");\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "cur"));
}

// The `if` twin of the issue's repro: the same reassignment, the same
// `return cur;`, but in an `if` arm rather than a loop body. It has no back
// edge, so the traversal visits the arm BEFORE the return — and the lint was
// already silent there. Pinning it keeps the fix's blast radius honest: the
// loop form and the `if` form must now agree (both silent).
BEZEL_TEST(ownership_28_3_8392,
           local_returned_after_if_does_not_draw_must_use_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module P;\n"
        "public class Node\n"
        "  destructor\n"
        "    Console.WriteLine(\"bye\");\n"
        "  end destructor;\n"
        "end Node;\n"
        "public class Program\n"
        "  private static method Build(n: int): owned Node\n"
        "    var cur := new Node();\n"
        "    if n > 0 then\n"
        "      cur := new Node();\n"
        "    end;\n"
        "    return cur;\n"
        "  end Build;\n"
        "  public static method Main()\n"
        "    using var x := Build(3);\n"
        "    Console.WriteLine(\"ok\");\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "cur"));
}
