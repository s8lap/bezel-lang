// Spec-derived regression tests for issue #8757 — stdlib §4.6's
// `LinkedList<T>.First` / `.Last` are typed leniently.
//
// THE RULE. Spec §5.1 "Implicit conversions" (v0.359) lists the implicit
// reference conversions in a CLOSED list:
//
//   - **identity**: every type converts to itself;
//   - **class to base class**, transitively, up to `Object`;
//   - **any type to `Object`**;
//   - **a type to an interface it implements or conforms to**;
//   - **interface to extended interface**, directly or transitively;
//   - **a type parameter to an interface or class its constraints guarantee
//     conformance to**;
//   - the **variance-induced conversions** of §12.9.
//
// and adds: "**Nullable conversions.** For any type `T`, `T` implicitly
// converts to `T?` … Each implicit conversion of this section also **lifts**
// to the nullable forms: `S?` implicitly converts to `T?` whenever `S`
// implicitly converts to `T`".
//
// stdlib §4.6 (docs/Bezel_Standard_Library.md) declares, on `LinkedList<T>`:
//
//   public pure readonly property First: T?;
//   public pure readonly property Last: T?;
//   public pure readonly property FirstNode: LinkedListNode<T>?;
//   public pure readonly property LastNode: LinkedListNode<T>?;
//
// so `l.First` is `T?` — the ELEMENT type, nullable — and `l.FirstNode` is
// `LinkedListNode<T>?`. §12.4 (v0.338) closes every compiler-known class to
// inheritance unless its text opens it, and `LinkedListNode` is not opened
// (the compiler refuses `class MyNode: LinkedListNode<Item>` with "cannot
// inherit from compiler-known class `LinkedListNode` … (§12.4, §31.4)"), so
// no type is a subtype of `LinkedListNode<T>` but `LinkedListNode<T>` itself.
//
// WHAT WAS BROKEN. `ExprTC::checkAssignable` (src/sema/ExprTypecheck.cpp) —
// the ONE §5.1 conversion check every written-declared-type position runs —
// decides a Named↔Named pair through a DECLARATION-KEYED check that needs
// BOTH `from->declaration` and `to->declaration`. `LinkedListNode<T>` is a
// compiler-known class with no user TypeSymbol (a declaration-less Named), so
// that check never sees the pair; the builtin-generic block refuses a
// non-convertible source only at the SPAN, COLLECTION and ITERABLE-INTERFACE
// heads, and `LinkedListNode` is in none of them; and the function's final
// "Mixed / generic-parameter / null-literal cases: skip" ACCEPTED the pair.
//
// MEASURED on the base (r30-c1 @ 983559b09, and again on the rebased base
// e97a8efbc): the issue's repro
// `var c: LinkedListNode<int>? := m.First;` (with `m: LinkedList<int>`)
// COMPILED — `m.First` is `int?`, and §5.1 lists no conversion from `int` to
// `LinkedListNode<int>`, so the nullable lift does not apply either. The same
// shape was accepted at `LinkedListNode<Item>?` (a user-class source), at
// `Last` instead of `First`, at a `LinkedListNode<int>?` PARAMETER, and on
// the owning sibling `OwningLinkedList<T>` (stdlib §4.7). The issue's second
// shape, `var d: string := l.First;`, was refused with the §7 nullability
// message ("is nullable but the target type is non-nullable (§7)") although
// the failure is a §5.1 TYPE mismatch: `Item` does not implicitly convert to
// `string`, so §5.1's nullable lift does not apply and the assignment fails
// the §5.1 rule.
//
// Every row below is on `runAot` and is RED on the base: each program either
// COMPILES on the base (so `r.ran` is true and no diagnostic exists) or is
// refused there with the §7 message this fix replaces. A reject row asserts:
// did not run, rejected with a real source location, and the diagnostic names
// the §5.1 rule.
//
// The ADMITTING TWINS of these shapes — `l.First` at an `Item?` slot, `l.
// FirstNode` at a `LinkedListNode<Item>?` slot, `l.First` at an `Object?`
// slot, `m.First` at an `int` slot (the PURE nullability drop, still §7),
// `null` at a `LinkedListNode<int>?` slot, and `l.FirstNode` at an `Item?`
// slot (the issue's own "correctly rejected" control) — are base-green by
// construction and are NOT gtest rows; they are in the lane's
// `corpus-candidates/` directory with the §5.1 sentence that decides each one
// (lane contract §7, RUN.md "Base-green rows go to the CORPUS, not gtest").

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The §5.1 rejection wording `reportNotConvertible` emits, and the reason this
// fix attaches to it for a `LinkedListNode<T>` target.
constexpr const char* kNotConvertible =
    "not implicitly convertible to the target type (§5.1)";
constexpr const char* kNodeClassReason =
    "`LinkedListNode<T>` is a compiler-known class (stdlib §4.6)";
constexpr const char* kClosedToInheritance =
    "the class is closed to inheritance (§12.4)";

// The §7 nullability message this fix REPLACES for a type mismatch. Asserted
// absent on the rows that are a type mismatch, so the row pins the RULE, not
// merely the rejection.
constexpr const char* kNullableMessage =
    "is nullable but the target type is non-nullable (§7)";

// A `LinkedListNode<T>` target admits only itself (identity, and the §5.1
// nullable lift) — so every source that is not a node is refused with the
// §5.1 rule.
void expectNodeTargetRejected(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNodeClassReason));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClosedToInheritance));
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// The issue's second shape: a `T?` value at a `string` slot. The failure is a
// §5.1 TYPE mismatch (`Item` does not implicitly convert to `string`), so the
// diagnostic must be the §5.1 one and NOT the §7 nullability one.
void expectTypeMismatchRejected(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotConvertible));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kNullableMessage), 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

}  // namespace

// REJECT — THE ISSUE'S REPRO (its four statements, verbatim; a `Main` is added
// so the program has an entry point). On the base the compiler reported the
// two §7 nullability messages for `b` and `d` and ACCEPTED `c`; on this tip
// `b` is still the §7 nullability drop (`int?` → `int` — the base types
// match) while `c` and `d` are §5.1 type mismatches.
BEZEL_TEST(ownership_linkedlist_first_type_8757,
           issue_repro_linkedlist_first_rows_spec_5_1) {
    auto r = runAot(R"bzl(module Conf;
public class Item
  public constructor() end constructor;
end Item;
public class Prog
  public method Take(l: LinkedList<Item>, m: LinkedList<int>)
    var a: Item? := l.First;
    var b: int := m.First;
    var c: LinkedListNode<int>? := m.First;
    var d: string := l.First;
  end Take;
  public static method Main(): int
    return 0;
  end Main;
end Prog;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // `c` and `d` are type errors under §5.1 …
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kNotConvertible), 2);
    // … and `b` is the §7 nullability drop the issue leaves alone.
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kNullableMessage), 1);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// REJECT — THE ISSUE'S `c` ROW: `m.First` is `int?` (stdlib §4.6's
// `First: T?`), and §5.1 lists no conversion from `int` to
// `LinkedListNode<int>`, so the nullable lift does not apply either. On the
// base this program COMPILED.
BEZEL_TEST(ownership_linkedlist_first_type_8757,
           issue_repro_linkedlist_first_into_node_slot_rejected_spec_5_1) {
    auto r = runAot(R"bzl(module Conf;
public class Prog
  public method Take(m: LinkedList<int>)
    var c: LinkedListNode<int>? := m.First;
  end Take;
  public static method Main(): int
    return 0;
  end Main;
end Prog;
)bzl");
    expectNodeTargetRejected(r);
}

// REJECT — THE ISSUE'S `d` ROW: `l.First` is `Item?`, and `Item` does not
// implicitly convert to `string` (§5.1's reference-conversion list has no
// class→primitive row; §4.6 forbids an implicit to-string), so this is a §5.1
// TYPE mismatch and not a §7 nullability drop. On the base it was refused
// with the §7 message.
BEZEL_TEST(ownership_linkedlist_first_type_8757,
           issue_repro_linkedlist_first_into_string_rejected_spec_5_1) {
    auto r = runAot(R"bzl(module Conf;
public class Item
  public constructor() end constructor;
end Item;
public class Prog
  public method Take(l: LinkedList<Item>)
    var d: string := l.First;
  end Take;
  public static method Main(): int
    return 0;
  end Main;
end Prog;
)bzl");
    expectTypeMismatchRejected(r);
}

// REJECT — THE SIBLING MEMBER stdlib §4.6 names: `Last: T?` beside `First:
// T?`. The same rule, the same rejection. On the base this program COMPILED.
BEZEL_TEST(ownership_linkedlist_first_type_8757,
           linkedlist_last_into_node_slot_rejected_spec_5_1) {
    auto r = runAot(R"bzl(module Conf;
public class Prog
  public method Take(m: LinkedList<int>)
    var c: LinkedListNode<int>? := m.Last;
  end Take;
  public static method Main(): int
    return 0;
  end Main;
end Prog;
)bzl");
    expectNodeTargetRejected(r);
}

// REJECT — A CLASS element: `l.First` is `Item?` and `Item` is not a subtype
// of `LinkedListNode<Item>` (the class is closed to inheritance, §12.4), so
// the §5.1 list admits no conversion. On the base this program COMPILED.
BEZEL_TEST(ownership_linkedlist_first_type_8757,
           linkedlist_first_class_element_into_node_slot_rejected_spec_5_1) {
    auto r = runAot(R"bzl(module Conf;
public class Item
  public constructor() end constructor;
end Item;
public class Prog
  public method Take(l: LinkedList<Item>)
    var c: LinkedListNode<Item>? := l.First;
  end Take;
  public static method Main(): int
    return 0;
  end Main;
end Prog;
)bzl");
    expectNodeTargetRejected(r);
}

// REJECT — AT A PARAMETER, not only at a local initializer: `checkAssignable`
// is the ONE §5.1 check every written-declared-type position runs, so the
// argument position is rejected too. On the base this program COMPILED.
BEZEL_TEST(ownership_linkedlist_first_type_8757,
           linkedlist_first_into_node_parameter_rejected_spec_5_1) {
    auto r = runAot(R"bzl(module Conf;
public class Prog
  public method TakeNode(n: LinkedListNode<int>?)
  end TakeNode;
  public method Take(m: LinkedList<int>)
    TakeNode(m.First);
  end Take;
  public static method Main(): int
    return 0;
  end Main;
end Prog;
)bzl");
    expectNodeTargetRejected(r);
}

// REJECT — THE OWNING SIBLING (stdlib §4.7): `OwningLinkedList<T>.First: T?`
// is declared beside §4.6's, so the same rule decides it. On the base this
// program COMPILED.
BEZEL_TEST(ownership_linkedlist_first_type_8757,
           owning_linkedlist_first_into_node_slot_rejected_spec_5_1) {
    auto r = runAot(R"bzl(module Conf;
public class Item
  public constructor() end constructor;
end Item;
public class Prog
  public method Take(o: OwningLinkedList<Item>)
    var c: LinkedListNode<Item>? := o.First;
  end Take;
  public static method Main(): int
    return 0;
  end Main;
end Prog;
)bzl");
    expectNodeTargetRejected(r);
}
