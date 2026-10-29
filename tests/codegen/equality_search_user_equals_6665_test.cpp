// Regression tests for #6665 (stdlib §4 *Equality search at an element type with
// no equality*, spec §9.2, §3.6, §25.2, v0.339): the equality search -- `Contains`
// and `Remove(item)` of `Array<T>` and `LinkedList<T>`, `Stack<T>.Contains`,
// `Queue<T>.Contains`, `Array<T>.IndexOf`, `LinkedList<T>.Find`, the span family's
// `IndexOf`, `Contains` and `SequenceEqual`, and `x in xs` -- "compare an
// argument with stored elements by `Equals`". A record or value-like choice that
// declares its own-typed `Equals` (with `GetHashCode`) was compared STRUCTURALLY
// by these members, so `Array<B>.Contains(new B(35))` over `B(5)` answered False
// under an `X mod 10` equality while `Set<B>.Contains` answered True.
//
// Every row loops 200x under the checked oracle and asserts stdout. Rows that
// must see the body run use an `Equals` that prints; the data-reading rows use
// `X mod 10`, so 5 / 15 / 25 / 35 are one key and 16 / 26 another.
//
// Not covered here, by design: `Array<T>.LastIndexOf` has its own tests
// (array_lastindexof_7671), and `.Contains` / `.Remove` through
// `ICollection<T>` / `IList<T>` is refused in codegen (#853); neither is this
// lowering.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kMembers =
    "  public method Equals(other: B): bool => (X mod 10) = (other.X mod 10);\n"
    "  public method GetHashCode(): int => (X mod 10) + 500;\n"
    "  public static method operator =(a: B, b: B): bool =>\n"
    "    (a.X mod 10) = (b.X mod 10);\n"
    "  public static method operator <>(a: B, b: B): bool =>\n"
    "    (a.X mod 10) <> (b.X mod 10);\n";

// The same members with a printing `Equals`: the printed line is the proof that
// the declared body ran (a structural compare never prints).
const char* kPrintingMembers =
    "  public method Equals(other: B): bool\n"
    "    Console.WriteLine(\"eq\");\n"
    "    return (X mod 10) = (other.X mod 10);\n"
    "  end Equals;\n"
    "  public method GetHashCode(): int => (X mod 10) + 500;\n"
    "  public static method operator =(a: B, b: B): bool =>\n"
    "    (a.X mod 10) = (b.X mod 10);\n"
    "  public static method operator <>(a: B, b: B): bool =>\n"
    "    (a.X mod 10) <> (b.X mod 10);\n";

std::string bodyDecl(const char* members = kMembers) {
    return std::string("module P;\n"
                       "public record B\n"
                       "  public readonly property X: int;\n"
                       "  public constructor(x: int)\n"
                       "    X := x;\n"
                       "  end constructor;\n") + members + "end B;\n";
}

std::string posDecl() {
    return std::string("module P;\npublic record B(X: int)\n") + kMembers +
           "end B;\n";
}

// A value-like choice: every pair is "equal", which no structural compare of
// distinct payloads can reproduce.
std::string choiceDecl() {
    return "module P;\n"
           "public choice B\n"
           "  A(X: int),\n"
           "  Z;\n"
           "  public method Equals(other: B): bool => true;\n"
           "  public method GetHashCode(): int => 0;\n"
           "  public static method operator =(a: B, b: B): bool => true;\n"
           "  public static method operator <>(a: B, b: B): bool => false;\n"
           "end B;\n";
}

std::string classDecl() {
    return "module P;\n"
           "public class B\n"
           "  public readonly property X: int;\n"
           "  public constructor(x: int)\n"
           "    X := x;\n"
           "  end constructor;\n"
           "  public override method Equals(other: Object?): bool\n"
           "    if other is B then\n"
           "      var b := other as B;\n"
           "      return (X mod 10) = (b!.X mod 10);\n"
           "    end;\n"
           "    return false;\n"
           "  end Equals;\n"
           "  public override method GetHashCode(): int => (X mod 10) + 500;\n"
           "end B;\n";
}

std::string autoDecl() { return "module P;\npublic record B(X: int);\n"; }

std::string prog(const std::string& decl, const std::string& stmts,
                 const std::string& generics = "") {
    return decl + generics +
           "public class Program\n"
           "  public static method Main()\n"
           "    var n := 0;\n"
           "    for var j in 0..<200 do\n" + stmts +
           "    end for;\n"
           "    Console.WriteLine(n);\n"
           "  end Main;\n"
           "end Program;\n";
}

// Constructor spellings of the element kinds, by the value the `mod 10` family
// reads. `mk(5)` etc. are built as locals so a class element is a borrow of a
// live owner.
std::string mk(const std::string& kind, int v) {
    if (kind == "choice") return "B.A(" + std::to_string(v) + ")";
    return "new B(" + std::to_string(v) + ")";
}

// Locals e1 e2 e3 (5, 16, 25), the query q (35) and o1..o3 (35, 26, 15), typed
// `et` (`B` or `B?`).
std::string locals(const std::string& kind, const std::string& et) {
    std::string s;
    auto def = [&](const char* nm, int v) {
        s += std::string("      var ") + nm + ": " + et + " := " + mk(kind, v) +
             ";\n";
    };
    def("e1", 5); def("e2", 16); def("e3", 25);
    def("q", 35);
    def("o1", 35); def("o2", 26); def("o3", 15);
    return s;
}

std::string fill(const char* cls, const char* add, const std::string& et) {
    return std::string("      using var c := new ") + cls + "<" + et + ">();\n"
           "      c." + add + "(e1); c." + add + "(e2); c." + add + "(e3);\n";
}

std::string spanOther(const std::string& et) {
    return "      using var d := new Array<" + et + ">();\n"
           "      d.Add(o1); d.Add(o2); d.Add(o3);\n";
}

const std::string kAC = "      if c.Contains(q) then n := n + 1; end;\n";
const std::string kAI = "      if c.IndexOf(q) = 0 then n := n + 1; end;\n";
const std::string kARem =
    "      if c.Remove(q) and c.Count = 2 then n := n + 1; end;\n";
const std::string kAIn = "      if q in c then n := n + 1; end;\n";
const std::string kLC = "      if c.Contains(q) then n := n + 1; end;\n";
const std::string kLRem =
    "      if c.Remove(q) and c.Count = 2 then n := n + 1; end;\n";
const std::string kLFind =
    "      var f := c.Find(q);\n"
    "      if f <> null then n := n + 1; end;\n";
const std::string kLIn = "      if q in c then n := n + 1; end;\n";

JitResult go(const std::string& decl, const std::string& kind,
             const std::string& et, const std::string& cls, const char* add,
             const std::string& ops) {
    return runAotChecked(
        prog(decl, locals(kind, et) + fill(cls.c_str(), add, et) + ops));
}

JitResult goSpan(const std::string& decl, const std::string& kind,
                 const std::string& et, const std::string& ops) {
    return runAotChecked(prog(decl, locals(kind, et) + fill("Array", "Add", et) +
                                        spanOther(et) + ops));
}

void expect200(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

}  // namespace

// ---- Array<T>: Contains / IndexOf / Remove / in (the issue's shape) ---------

BEZEL_TEST(equality_search_user_equals_9_2_6665, body_array_contains_issue_repro) {
    expect200(go(bodyDecl(), "body", "B", "Array", "Add", kAC));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_array_indexof) {
    expect200(go(bodyDecl(), "body", "B", "Array", "Add", kAI));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_array_remove) {
    expect200(go(bodyDecl(), "body", "B", "Array", "Add", kARem));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_array_in) {
    expect200(go(bodyDecl(), "body", "B", "Array", "Add", kAIn));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, positional_array_contains) {
    expect200(go(posDecl(), "pos", "B", "Array", "Add", kAC));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, positional_array_indexof) {
    expect200(go(posDecl(), "pos", "B", "Array", "Add", kAI));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, choice_array_contains) {
    expect200(go(choiceDecl(), "choice", "B", "Array", "Add", kAC));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, choice_array_indexof) {
    expect200(go(choiceDecl(), "choice", "B", "Array", "Add", kAI));
}

// ---- nullable element: the declared `Equals` when both are present ---------

BEZEL_TEST(equality_search_user_equals_9_2_6665, nullable_body_array_contains) {
    expect200(go(bodyDecl(), "body", "B?", "Array", "Add", kAC));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, nullable_positional_array_indexof) {
    expect200(go(posDecl(), "pos", "B?", "Array", "Add", kAI));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, nullable_body_in) {
    expect200(go(bodyDecl(), "body", "B?", "Array", "Add", kAIn));
}

// §8.4 null-safe rule unchanged: null = null, null <> a present value, and the
// declared member is NOT called with an absent operand (it would read junk).
BEZEL_TEST(equality_search_user_equals_9_2_6665, nullable_null_query_and_null_element) {
    JitResult r = runAotChecked(prog(bodyDecl(),
        "      var nul: B? := null;\n"
        "      using var c := new Array<B?>();\n"
        "      c.Add(new B(5)); c.Add(nul);\n"
        "      using var d := new Array<B?>();\n"
        "      d.Add(new B(5));\n"
        "      if c.Contains(nul) then n := n + 1; end;\n"
        "      if c.IndexOf(nul) = 1 then n := n + 1; end;\n"
        "      if not d.Contains(nul) then n := n + 1; end;\n"
        "      if c.Contains(new B(15)) then n := n + 1; end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("800\n"));
}

// ---- LinkedList<T> -----------------------------------------------------------

BEZEL_TEST(equality_search_user_equals_9_2_6665, body_linkedlist_contains) {
    expect200(go(bodyDecl(), "body", "B", "LinkedList", "Add", kLC));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_linkedlist_remove) {
    expect200(go(bodyDecl(), "body", "B", "LinkedList", "Add", kLRem));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_linkedlist_find) {
    expect200(go(bodyDecl(), "body", "B", "LinkedList", "Add", kLFind));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_linkedlist_in) {
    expect200(go(bodyDecl(), "body", "B", "LinkedList", "Add", kLIn));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, choice_linkedlist_find) {
    expect200(go(choiceDecl(), "choice", "B", "LinkedList", "Add", kLFind));
}

// ---- Stack<T> / Queue<T> -----------------------------------------------------

BEZEL_TEST(equality_search_user_equals_9_2_6665, body_stack_contains) {
    expect200(go(bodyDecl(), "body", "B", "Stack", "Push", kLC));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_queue_contains) {
    expect200(go(bodyDecl(), "body", "B", "Queue", "Enqueue", kLC));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, positional_queue_contains) {
    expect200(go(posDecl(), "pos", "B", "Queue", "Enqueue", kLC));
}

// ---- spans: IndexOf / Contains / SequenceEqual -------------------------------

BEZEL_TEST(equality_search_user_equals_9_2_6665, body_span_indexof) {
    expect200(goSpan(bodyDecl(), "body", "B",
        "      if c.AsSpan().IndexOf(q) = 0 then n := n + 1; end;\n"));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_span_contains) {
    expect200(goSpan(bodyDecl(), "body", "B",
        "      if c.AsSpan().Contains(q) then n := n + 1; end;\n"));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_span_sequenceequal) {
    expect200(goSpan(bodyDecl(), "body", "B",
        "      if c.AsSpan().SequenceEqual(d.AsSpan()) then n := n + 1; end;\n"));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_readonlyspan_indexof) {
    expect200(goSpan(bodyDecl(), "body", "B",
        "      if c.AsReadOnlySpan().IndexOf(q) = 0 then n := n + 1; end;\n"));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, body_readonlyspan_sequenceequal) {
    expect200(goSpan(bodyDecl(), "body", "B",
        "      if c.AsReadOnlySpan().SequenceEqual(d.AsReadOnlySpan()) then\n"
        "        n := n + 1;\n"
        "      end;\n"));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, choice_span_sequenceequal) {
    expect200(goSpan(choiceDecl(), "choice", "B",
        "      if c.AsSpan().SequenceEqual(d.AsSpan()) then n := n + 1; end;\n"));
}

// ---- the declared body actually RUNS (printing Equals) -----------------------

// One search over [B(5), B(16)] for B(35) calls the declared `Equals` once (5 ~
// 35 matches first), so each iteration prints exactly one "eq". A structural
// compare prints none.
BEZEL_TEST(equality_search_user_equals_9_2_6665, printing_equals_runs_in_array_contains) {
    JitResult r = runAotChecked(
        "module P;\n"
        "public record B\n"
        "  public readonly property X: int;\n"
        "  public constructor(x: int)\n"
        "    X := x;\n"
        "  end constructor;\n" +
        std::string(kPrintingMembers) + "end B;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var a := new Array<B>();\n"
        "    a.Add(new B(5)); a.Add(new B(16));\n"
        "    for var j in 0..<200 do\n"
        "      if a.Contains(new B(35)) then Console.WriteLine(\"hit\"); end;\n"
        "    end for;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    std::string want;
    for (int i = 0; i < 200; ++i) want += "eq\nhit\n";
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// The search must NOT call `Equals` for an absent nullable operand.
BEZEL_TEST(equality_search_user_equals_9_2_6665, printing_equals_not_called_for_absent_operand) {
    JitResult r = runAotChecked(
        "module P;\n"
        "public record B\n"
        "  public readonly property X: int;\n"
        "  public constructor(x: int)\n"
        "    X := x;\n"
        "  end constructor;\n" +
        std::string(kPrintingMembers) + "end B;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var nul: B? := null;\n"
        "    using var a := new Array<B?>();\n"
        "    a.Add(nul);\n"
        "    for var j in 0..<200 do\n"
        "      if a.Contains(nul) then Console.WriteLine(\"hit\"); end;\n"
        "    end for;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    std::string want;
    for (int i = 0; i < 200; ++i) want += "hit\n";
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// ---- generic body: the body's own comparison, read at the instantiation ------

BEZEL_TEST(equality_search_user_equals_9_2_6665, generic_body_contains_reads_instantiation) {
    JitResult r = runAotChecked(prog(bodyDecl(),
        "      using var a := new Array<B>();\n"
        "      a.Add(new B(5)); a.Add(new B(16));\n"
        "      if Util.Has<B>(a, new B(35)) then n := n + 1; end;\n"
        "      if not Util.Has<B>(a, new B(7)) then n := n + 1; end;\n",
        "public class Util\n"
        "  public static method Has<T>(xs: Array<T>, q: T): bool\n"
        "    return xs.Contains(q);\n"
        "  end Has;\n"
        "end Util;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("400\n"));
}

// ---- controls: no regression where the answer was already right --------------

// A class that overrides `Equals(Object?)` (was already dispatched virtually).
BEZEL_TEST(equality_search_user_equals_9_2_6665, control_class_override_array_contains) {
    expect200(go(classDecl(), "class", "B", "Array", "Add", kAC));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, control_class_override_linkedlist_find) {
    expect200(go(classDecl(), "class", "B", "LinkedList", "Add", kLFind));
}
// An auto-equality record: structural, 25 present and 7 absent.
BEZEL_TEST(equality_search_user_equals_9_2_6665, control_auto_record_structural) {
    JitResult r = runAotChecked(prog(autoDecl(),
        "      using var a := new Array<B>();\n"
        "      a.Add(new B(5)); a.Add(new B(16)); a.Add(new B(25));\n"
        "      if a.Contains(new B(25)) then n := n + 1; end;\n"
        "      if a.IndexOf(new B(25)) = 2 then n := n + 1; end;\n"
        "      if not a.Contains(new B(35)) then n := n + 1; end;\n"
        "      if new B(16) in a then n := n + 1; end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("800\n"));
}
// The declared member answers FALSE on every pair: structural-equal operands
// must NOT be found (the declared `Equals` is the equality, even "never").
BEZEL_TEST(equality_search_user_equals_9_2_6665, never_equal_declared_equals_finds_nothing) {
    JitResult r = runAotChecked(prog(
        "module P;\n"
        "public record B(X: int)\n"
        "  public method Equals(other: B): bool => false;\n"
        "  public method GetHashCode(): int => 0;\n"
        "  public static method operator =(a: B, b: B): bool => false;\n"
        "  public static method operator <>(a: B, b: B): bool => true;\n"
        "end B;\n",
        "      using var a := new Array<B>();\n"
        "      a.Add(new B(5));\n"
        "      if not a.Contains(new B(5)) then n := n + 1; end;\n"
        "      if a.IndexOf(new B(5)) = -1 then n := n + 1; end;\n"
        "      if not a.Remove(new B(5)) and a.Count = 1 then n := n + 1; end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));
}

// A GENERIC record's declared pair is the instantiation's own functions (the
// #6089 reading the hash containers use): `G<string>` compares by `W` only.
BEZEL_TEST(equality_search_user_equals_9_2_6665, generic_record_declared_equals) {
    JitResult r = runAotChecked(
        "module P;\n"
        "public record G<T>(V: T, W: int)\n"
        "  public method Equals(other: G<T>): bool => W = other.W;\n"
        "  public method GetHashCode(): int => W;\n"
        "  public static method operator =(a: G<T>, b: G<T>): bool => a.W = b.W;\n"
        "  public static method operator <>(a: G<T>, b: G<T>): bool => a.W <> b.W;\n"
        "end G;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var n := 0;\n"
        "    for var j in 0..<200 do\n"
        "      using var a := new Array<G<string>>();\n"
        "      a.Add(new G<string>(\"a\", 1)); a.Add(new G<string>(\"b\", 2));\n"
        "      if a.Contains(new G<string>(\"zzz\", 2)) then n := n + 1; end;\n"
        "      if a.IndexOf(new G<string>(\"zzz\", 2)) = 1 then n := n + 1; end;\n"
        "      if not a.Contains(new G<string>(\"a\", 3)) then n := n + 1; end;\n"
        "      using var l := new LinkedList<G<string>?>();\n"
        "      l.Add(new G<string>(\"a\", 1));\n"
        "      if l.Contains(new G<string>(\"q\", 1)) then n := n + 1; end;\n"
        "    end for;\n"
        "    Console.WriteLine(n);\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("800\n"));
}

// ---- a THROWING declared `Equals` ends the search (§31.2) -------------------
// §31.2 (v0.339): "The arms are tried in order; the first arm whose pattern
// matches the thrown exception ... is selected, its body runs, and exception
// handling completes." The search calls the declared `Equals` on B(1), then on
// B(99), which throws; the element after it, B(3), must never be compared (its
// `Equals` prints "LATE"), the member must not return a result, the `try`'s own
// catch arm must run, and the loop then continues normally. Without the pending
// check after the call the search ran on to B(3), answered False/-1/null, and
// the exception surfaced at a LATER unrelated `try` or ended the program.
namespace {

std::string throwingProg(const std::string& et, const std::string& cls,
                         const std::string& op) {
    return "module P;\n"
           "public record B(X: int)\n"
           "  public method Equals(other: B): bool\n"
           "    if X = 3 then Console.WriteLine(\"LATE\"); end;\n"
           "    if X = 99 then\n"
           "      throw new InvalidOperationException(\"boom\");\n"
           "    end;\n"
           "    return false;\n"
           "  end Equals;\n"
           "  public method GetHashCode(): int => 0;\n"
           "  public static method operator =(a: B, b: B): bool => false;\n"
           "  public static method operator <>(a: B, b: B): bool => true;\n"
           "end B;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    var n := 0;\n"
           "    var caught := 0;\n"
           "    var after := 0;\n"
           "    for var j in 0..<200 do\n"
           "      using var c := new " + cls + "<" + et + ">();\n"
           "      c.Add(new B(1)); c.Add(new B(99)); c.Add(new B(3));\n"
           "      var q: " + et + " := new B(7);\n"
           "      try\n" + op +
           "        n := n + 1000;\n"
           "      catch\n"
           "        var ex: InvalidOperationException => do caught := caught + 1; end,\n"
           "      end try;\n"
           "      after := after + 1;\n"
           "    end for;\n"
           "    Console.WriteLine(caught);\n"
           "    Console.WriteLine(n);\n"
           "    Console.WriteLine(after);\n"
           "  end Main;\n"
           "end Program;\n";
}

void expectCaughtEveryTime(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n0\n200\n"));
}

}  // namespace

BEZEL_TEST(equality_search_user_equals_9_2_6665, throwing_equals_array_contains_31_2) {
    expectCaughtEveryTime(runAotChecked(throwingProg("B", "Array",
        "        if c.Contains(q) then n := n + 1; end;\n")));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, throwing_equals_array_remove_31_2) {
    expectCaughtEveryTime(runAotChecked(throwingProg("B", "Array",
        "        if c.Remove(q) then n := n + 1; end;\n")));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, throwing_equals_array_indexof_31_2) {
    expectCaughtEveryTime(runAotChecked(throwingProg("B", "Array",
        "        if c.IndexOf(q) >= 0 then n := n + 1; end;\n")));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, throwing_equals_in_31_2) {
    expectCaughtEveryTime(runAotChecked(throwingProg("B", "Array",
        "        if q in c then n := n + 1; end;\n")));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, throwing_equals_span_contains_31_2) {
    expectCaughtEveryTime(runAotChecked(throwingProg("B", "Array",
        "        if c.AsSpan().Contains(q) then n := n + 1; end;\n")));
}
BEZEL_TEST(equality_search_user_equals_9_2_6665, throwing_equals_linkedlist_find_31_2) {
    expectCaughtEveryTime(runAotChecked(throwingProg("B", "LinkedList",
        "        var f := c.Find(q);\n"
        "        if f <> null then n := n + 1; end;\n")));
}
// The nullable-element form calls the member on its both-present edge.
BEZEL_TEST(equality_search_user_equals_9_2_6665, throwing_equals_nullable_array_contains_31_2) {
    expectCaughtEveryTime(runAotChecked(throwingProg("B?", "Array",
        "        if c.Contains(q) then n := n + 1; end;\n")));
}
