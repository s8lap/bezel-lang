// Regression tests for #8100: the §9.4 receiver-store rule missed two shapes
// in a RECORD member body.
//
//   1. the `++`/`--` form — §17.6's desugaring makes `Raw++` a store to
//      `Raw`, so it is "the same store by other syntax" (§9.4), but the
//      IncrementStmt arm never reached checkRecordReceiverStore;
//   2. a store through a RECORD-typed member (`I.X := value`, `I` a
//      record-typed property or field) — the chain rooted at `this` reaches
//      the nested record's storage WITHOUT LEAVING THE VALUE, and a record
//      component is stored inline in its holder, so that storage IS the
//      receiver's own storage.
//
// Spec §9.4 (v0.355), "Value receivers are borrows": "Consequently a store
// that would land in the **receiver's own storage** is a compile error at the
// member body ... The receiver's own storage is what an lvalue chain rooted at
// `this` reaches without leaving the value ... The rule keys on **where the
// store would land** — never on the chain's root or the statement's form".
// The two boundaries the same section states are pinned by the ACCEPT rows: a
// constructor body is not a receiver position, and a store beyond a class- or
// interface-typed hop lands in the borrowed heap object and is legal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}
// The §9.4 diagnostic's stable core, and the increment/decrement wording the
// same check selects for the `++`/`--` spelling.
constexpr const char* kStore = "to the receiver's own storage";
constexpr const char* kIncr = "cannot increment/decrement to the receiver's own storage";
constexpr const char* kSt = "cannot store to the receiver's own storage";
constexpr const char* kSec = "(§9.4)";
}  // namespace

// #8100 gap 1: `Raw++` in a SETTER body. §17.6's desugaring makes it a store
// to the receiver's own storage; §9.4 rejects it there. (Accepted before the
// fix, and it wrote through to the borrowed receiver: the program printed 3.)
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           setter_increment_own_storage_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public property D: int "
        "    get => Raw; "
        "    set => do Raw++; end; "
        "  end D; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    var g := new Gauge(2); "
        "    g.D := 4; "
        "    Console.WriteLine(g.Raw); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kIncr));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// #8100 gap 1: the METHOD twin of the row above.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           method_increment_own_storage_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public method M() Raw++; end M; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    var g := new Gauge(2); "
        "    g.M(); "
        "    Console.WriteLine(g.Raw); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kIncr));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// The `--` spelling of the same store.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           method_decrement_own_storage_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public method M() Raw--; end M; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    var g := new Gauge(2); "
        "    g.M(); "
        "    Console.WriteLine(g.Raw); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kIncr));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// The explicit `this.` spelling of the same store.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           method_explicit_this_increment_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public method M() do this.Raw++; end; end M; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    var g := new Gauge(2); "
        "    g.M(); "
        "    Console.WriteLine(g.Raw); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kIncr));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// #8100 gap 2: a store through a RECORD-typed property. `I.X` is the nested
// record's storage, and a record component is stored inline in its holder,
// so the store lands in the receiver's own storage. (Accepted before the fix;
// the program printed 4.)
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           setter_nested_record_field_store_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Inner "
        "  public property X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end Inner; "
        "public record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) "
        "    I := i; "
        "  end constructor; "
        "  public property D: int "
        "    get => I.X; "
        "    set => do I.X := value; end; "
        "  end D; "
        "end Outer; "
        "public class Program "
        "  public static method Main() "
        "    var o := new Outer(new Inner(1)); "
        "    o.D := 4; "
        "    Console.WriteLine(o.I.X); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kSt));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// #8100 gap 2: the METHOD twin of the row above.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           method_nested_record_field_store_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Inner "
        "  public property X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end Inner; "
        "public record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) "
        "    I := i; "
        "  end constructor; "
        "  public method SetD(v: int) I.X := v; end SetD; "
        "end Outer; "
        "public class Program "
        "  public static method Main() "
        "    var o := new Outer(new Inner(1)); "
        "    o.SetD(4); "
        "    Console.WriteLine(o.I.X); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kSt));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// #8100 gap 2: the compound form (`+=`) of the same store — §6.6's desugaring
// puts the assignment semantics on the same site.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           setter_nested_record_field_compound_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Inner "
        "  public property X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end Inner; "
        "public record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) "
        "    I := i; "
        "  end constructor; "
        "  public property D: int "
        "    get => I.X; "
        "    set => do I.X += value; end; "
        "  end D; "
        "end Outer; "
        "public class Program "
        "  public static method Main() "
        "    var o := new Outer(new Inner(1)); "
        "    o.D := 4; "
        "    Console.WriteLine(o.I.X); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kSt));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// #8100 gaps 1 and 2 together: the increment form of the nested store.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           method_nested_record_field_increment_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Inner "
        "  public property X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end Inner; "
        "public record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) "
        "    I := i; "
        "  end constructor; "
        "  public method Bump() do I.X++; end; end Bump; "
        "end Outer; "
        "public class Program "
        "  public static method Main() "
        "    var o := new Outer(new Inner(1)); "
        "    o.Bump(); "
        "    Console.WriteLine(o.I.X); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kIncr));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// The explicit `this.` spelling of the nested store.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           method_explicit_this_nested_store_rejected_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Inner "
        "  public property X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end Inner; "
        "public record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) "
        "    I := i; "
        "  end constructor; "
        "  public method SetD(v: int) do this.I.X := v; end; end SetD; "
        "end Outer; "
        "public class Program "
        "  public static method Main() "
        "    var o := new Outer(new Inner(1)); "
        "    o.SetD(4); "
        "    Console.WriteLine(o.I.X); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kSt));
    BEZEL_EXPECT_TRUE(diagContains(r, kSec));
}

// §9.4's first boundary: "Constructor bodies are not receiver positions" —
// a constructor initializes the value under construction, so its stores to the
// record's own storage are legal. (The control for both new shapes.)
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           constructor_body_store_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw + 1; "
        "  end constructor; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    var g := new Gauge(2); "
        "    Console.WriteLine(g.Raw); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// A store into ANOTHER record's storage is not a receiver store: the chain
// does not start at `this` at all (§9.4 keys on where the store lands).
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           other_record_store_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Other "
        "  public property X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end Other; "
        "public record Gauge "
        "  public readonly property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public method Make(): int "
        "    var o := new Other(1); "
        "    o.X := Raw + 5; "
        "    return o.X; "
        "  end Make; "
        "  public property P: int "
        "    get => Raw; "
        "    set => do var o := new Other(0); o.X := value; Console.WriteLine(o.X); end; "
        "  end P; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    var g := new Gauge(2); "
        "    Console.WriteLine(g.Make()); "
        "    g.P := 9; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n9\n"));
}

// §9.4's second boundary, read with the new chain walk: a store to a STATIC
// is not a receiver store — a static is not reached through `this`.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           static_field_store_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Gauge "
        "  public static Count: int := 0; "
        "  public readonly property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public property D: int "
        "    get => Raw; "
        "    set => do Count := Count + value; end; "
        "  end D; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    var g := new Gauge(2); "
        "    g.D := 4; "
        "    Console.WriteLine(Gauge.Count); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// §9.4's mediating shape: a store beyond a class-typed hop lands in the
// borrowed heap object and is legal. The hop here is the FIRST hop of the
// chain.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           class_hop_store_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public class Cell "
        "  public property Value: int; "
        "  public constructor(value: int) "
        "    Value := value; "
        "  end constructor; "
        "end Cell; "
        "public record View "
        "  public readonly property Target: Cell; "
        "  public constructor(target: Cell) "
        "    Target := target; "
        "  end constructor; "
        "  public property Level: int "
        "    get => Target.Value; "
        "    set => Target.Value := value; "
        "  end Level; "
        "end View; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new Cell(0); "
        "    var v := new View(c); "
        "    v.Level := 7; "
        "    Console.WriteLine(c.Value); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// The class hop AFTER a record-typed hop: the chain leaves the receiver at
// the class-typed member, so the store beyond it lands in the borrowed heap
// object and is legal. This is the admitting twin of the nested-record rows
// above — the same shape (a record-typed member, then a member of it), with
// the second hop through a class-typed member instead of a record's field.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           class_hop_after_record_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public class Cell "
        "  public property Value: int; "
        "  public constructor(value: int) "
        "    Value := value; "
        "  end constructor; "
        "end Cell; "
        "public record Inner "
        "  public readonly property C: Cell; "
        "  public constructor(c: Cell) "
        "    C := c; "
        "  end constructor; "
        "end Inner; "
        "public record Outer "
        "  public property I: Inner; "
        "  public constructor(i: Inner) "
        "    I := i; "
        "  end constructor; "
        "  public method SetV(v: int) I.C.Value := v; end SetV; "
        "end Outer; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new Cell(0); "
        "    var o := new Outer(new Inner(c)); "
        "    o.SetV(4); "
        "    Console.WriteLine(c.Value); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// An increment on a LOCAL is not a receiver store: the local is the method
// body's own storage, not the receiver's.
BEZEL_TEST(record_receiver_store_incr_nested_8100,
           local_increment_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public method Bump(k: int): int "
        "    var t := k; "
        "    t++; "
        "    return t; "
        "  end Bump; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    var g := new Gauge(2); "
        "    Console.WriteLine(g.Bump(4)); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}
