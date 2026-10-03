// Regression tests for #5312 — the BUILTIN shape-(a) Try members did not apply
// the §22.4 `when true` narrowing to their `out` recipient, though a user
// method declared `out x: T? when true` did.
//
// WHAT THE DOCUMENTS REQUIRE.  Stdlib §1 (v0.313): "Every shape-(a) signature
// in this document carries the `when true` nullability postcondition on its
// out parameter (language spec §22.4) — `TryX(..., out item: T? when true):
// bool` — so in a region where the call is known `true`, the recipient is
// narrowed non-null and no `!` is needed; the clause is elided from the
// per-type listings below for brevity but is normative for **every**
// shape-(a) member in this document (the table's (a) row names them)."
// Spec §22.4 (v0.313): "At the call site, the compiler narrows the `out`
// recipient exactly as if the caller had performed an explicit null test: in
// any region where the call's `bool` result is statically known to equal the
// stated constant — the controlled branch of `if TryX(…, out var c) then`, an
// `and`-conjunct, an early-exit guard — the recipient is treated as non-null".
// Spec §7.11 (v0.313): arithmetic does not lift over `T?` — "narrow first".
// Pre-fix `if st.TryPop(out var v) then Console.WriteLine(v + 1); end;` was
// rejected ("operator `+` is not defined on a nullable value type").
//
// Members (stdlib §1 table, row (a)): Stack/OwningStack TryPop+TryPeek,
// Queue/OwningQueue TryDequeue+TryPeek, OwningArray/OwningLinkedList
// TryPopFirst+TryPopLast, Dictionary/SortedDictionary/OwningDictionary
// TryGetValue, OwningDictionary.TryPopValue, Channel.TryReceive,
// OwningSet.TryPop (sema-level accept row in tests/lit/sema), and
// IParsable<T>.TryParse (already narrowed, #1597). Value-typed payloads use
// `v + 1` (the strict §7.11 oracle); class payloads read a member (§7.4).
// The `then`-only scope is pinned by tests/lit/sema/builtin_shape_a_when_true_
// 5312__*.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kR =
    "module M; "
    "class R public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is R then var b := other as R; return b!.X = X; end; "
    "    return false; end Equals; "
    "  public override method GetHashCode(): int => X; end R; ";

std::string mainOf(const std::string& body) {
    return std::string(kR) + "class C public static method Main(): int " +
           body + " return 0; end Main; end C;";
}

void expect(const std::string& body, const std::string& want) {
    auto r = runAot(mainOf(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// ---- the issue's own repro: Stack<int>.TryPop ------------------------------

BEZEL_TEST(builtin_shape_a_when_true_5312, issue_repro_stack_trypop_out_var) {
    expect("var st := new Stack<int>(); st.Push(4); "
           "if st.TryPop(out var v) then Console.WriteLine(v + 1); end;",
           "5\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, stack_trypop_predeclared) {
    expect("var st := new Stack<int>(); st.Push(4); var w: int? := null; "
           "if st.TryPop(out w) then Console.WriteLine(w + 1); end;",
           "5\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, stack_trypeek) {
    expect("var st := new Stack<int>(); st.Push(4); "
           "if st.TryPeek(out var v) then Console.WriteLine(v + 1); end;",
           "5\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, queue_trydequeue_and_trypeek) {
    expect("var q := new Queue<int>(); q.Enqueue(4); "
           "if q.TryPeek(out var a) then Console.WriteLine(a + 1); end; "
           "if q.TryDequeue(out var b) then Console.WriteLine(b + 2); end;",
           "5\n6\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, queue_trydequeue_predeclared) {
    expect("var q := new Queue<int>(); q.Enqueue(4); var w: int? := null; "
           "if q.TryDequeue(out w) then Console.WriteLine(w + 1); end;",
           "5\n");
}

// ---- keyed lookup -------------------------------------------------------------

BEZEL_TEST(builtin_shape_a_when_true_5312, dictionary_trygetvalue) {
    expect("var d := new Dictionary<string, int>(); d.Add(\"a\", 1); "
           "if d.TryGetValue(\"a\", out var v) then "
           "  Console.WriteLine(v + 1); end;",
           "2\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, dictionary_trygetvalue_predeclared) {
    expect("var d := new Dictionary<string, int>(); d.Add(\"a\", 1); "
           "var w: int? := null; "
           "if d.TryGetValue(\"a\", out w) then "
           "  Console.WriteLine(w + 1); end;",
           "2\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, dictionary_string_value_member) {
    expect("var d := new Dictionary<string, string>(); d.Add(\"a\", \"abc\"); "
           "if d.TryGetValue(\"a\", out var v) then "
           "  Console.WriteLine(v.Length); end;",
           "3\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, sorteddictionary_trygetvalue) {
    expect("var d := new SortedDictionary<string, int>(); d.Add(\"a\", 1); "
           "if d.TryGetValue(\"a\", out var v) then "
           "  Console.WriteLine(v + 1); end;",
           "2\n");
}

// ---- channel ------------------------------------------------------------------

BEZEL_TEST(builtin_shape_a_when_true_5312, channel_tryreceive) {
    expect("using var ch := new Channel<int>(); "
           "using var tx := ch.NewSender(); tx.Send(5); "
           "if ch.TryReceive(out var v) then Console.WriteLine(v + 1); end;",
           "6\n");
}

// ---- owning collections (class payloads: a member read needs a non-null) ------

BEZEL_TEST(builtin_shape_a_when_true_5312, owningstack_trypeek_and_trypop) {
    expect("using var s := new OwningStack<R>(); s.Push(move (new R(1))); "
           "if s.TryPeek(out var a) then Console.WriteLine(a.X); end; "
           "if s.TryPop(out using var b) then Console.WriteLine(b.X); end;",
           "1\n1\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, owningqueue_trypeek_and_trydequeue) {
    expect("using var q := new OwningQueue<R>(); q.Enqueue(move (new R(2))); "
           "if q.TryPeek(out var a) then Console.WriteLine(a.X); end; "
           "if q.TryDequeue(out using var b) then Console.WriteLine(b.X); end;",
           "2\n2\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, owningarray_trypopfirst_and_last) {
    expect("using var a := new OwningArray<R>(); "
           "a.Add(move (new R(3))); a.Add(move (new R(4))); "
           "if a.TryPopFirst(out using var f) then Console.WriteLine(f.X); end; "
           "if a.TryPopLast(out using var l) then Console.WriteLine(l.X); end;",
           "3\n4\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, owninglinkedlist_trypopfirst_and_last) {
    expect("using var a := new OwningLinkedList<R>(); "
           "a.AddLast(move (new R(5))); a.AddLast(move (new R(6))); "
           "if a.TryPopFirst(out using var f) then Console.WriteLine(f.X); end; "
           "if a.TryPopLast(out using var l) then Console.WriteLine(l.X); end;",
           "5\n6\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, owningdictionary_trygetvalue_and_trypopvalue) {
    expect("using var d := new OwningDictionary<string, R>(); "
           "d.Add(\"k\", move (new R(7))); "
           "if d.TryGetValue(\"k\", out var a) then "
           "  Console.WriteLine(a.X); end; "
           "if d.TryPopValue(\"k\", out using var b) then "
           "  Console.WriteLine(b.X); end;",
           "7\n7\n");
}

// ---- composition (§22.4: "an `and`-conjunct") -------------------------------

BEZEL_TEST(builtin_shape_a_when_true_5312, and_conjunct_narrows_then) {
    expect("var st := new Stack<int>(); st.Push(8); "
           "if st.Count > 0 and st.TryPop(out var z) then "
           "  Console.WriteLine(z + 1000); end;",
           "1008\n");
}

// ---- runtime false path + the pre-existing `!` idiom ---------------------------

BEZEL_TEST(builtin_shape_a_when_true_5312, false_path_takes_else) {
    expect("var st := new Stack<int>(); "
           "if st.TryPop(out var v) then Console.WriteLine(v + 1); "
           "else Console.WriteLine(\"none\"); end;",
           "none\n");
}
BEZEL_TEST(builtin_shape_a_when_true_5312, null_forgiving_idiom_still_accepted) {
    // The blessed pre-fix spelling `v!` keeps compiling and running.
    expect("var st := new Stack<int>(); st.Push(4); "
           "if st.TryPop(out var v) then Console.WriteLine(v! + 1); end;",
           "5\n");
}
