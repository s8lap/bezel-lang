// Spec-derived regression test for issue #7500 — the ONE built-in parameter
// table (`kBuiltinParams`, src/common/BuiltinRegistry.cpp) carried NO ROW at
// all for a group of built-in heads, so a LEGAL §22.5 named argument at one
// of their members or constructors was REFUSED with "named arguments are not
// supported on this built-in member — the compiler carries no parameter names
// for it" — the very diagnostic the issue quotes.
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// spec §22.5 "Optional parameters and named arguments": "Named arguments may
//   appear in any order after positional arguments. Each supplied argument
//   binds exactly one parameter … Every parameter that declares no default
//   must be bound, positionally or by name." The rule reaches EVERY
//   construction site: "a call, a `new` expression, and a record construction
//   alike".
// stdlib §1 "Conventions": the parameter names are API surface ("parameter
//   names are API surface (named arguments, spec §22.5)").
// stdlib §4.7 / §4.11 / §4.17 / §4.18 / §6.1 / §6.2 / §6.4 / §6.10 declare
//   the names these rows carry:
//
//   §6.4  `Atomic`/`AtomicInteger`: constructor(initial: T);
//         Exchange(value: T); AtomicInteger.Add(delta: T).
//   §4.11 `Stack`/`OwningStack`: Push(item: T); Contains(item: T).
//         `Queue`/`OwningQueue`: Enqueue(item: T); Contains(item: T).
//   §4.18 `SortedSet`: Add(item: T); TryAdd(item: T); Contains(item: T);
//         Remove(item: T).
//   §4.17 `SortedDictionary`: ContainsKey(key: K); Contains(key: K);
//         GetOrNull(key: K); Remove(key: K).
//   §4.7  `OwningLinkedList`: AddFirst(item: T); AddLast(item: T);
//         Contains(item: T); Find(item: T); Remove(item: T).
//   §6.2  `Mutex`: TryAcquire(timeout: TimeSpan).
//   §6.1  `CancellationTokenSource`: CancelAfter(delay: TimeSpan).
//   §6.10 `TaskScope`: CancelAfter(delay: TimeSpan).
//
// ─── THE DEFECT (MEASURED on the pre-fix tree, build/bezelc -c) ───────────
//
//   new Atomic<int>(initial := 5)                 -> REFUSED
//   new AtomicInteger<int>(initial := 5)          -> REFUSED
//   a.Exchange(value := 6)                         -> REFUSED
//   ai.Add(delta := 3)                             -> REFUSED
//   s.Push(item := 5) / s.Enqueue(item := 5)       -> REFUSED
//   ss.Add(item := 5)                              -> REFUSED
//   sd.ContainsKey(key := 5) / sd.Remove(key := 5)-> REFUSED
//   l.AddLast(item := "a")                         -> REFUSED
//   m.TryAcquire(timeout := …)                     -> REFUSED
//   c.CancelAfter(delay := …)                      -> REFUSED
//   t.CancelAfter(delay := …)                      -> REFUSED
//
// every one of them with the SAME wording: "the compiler carries no
// parameter names for it". These heads ARE reserved built-in names, so the
// §22.5 binder CLAIMED the call and looked the declared names up in the ONE
// table — and the table carried no row for them.
//
// The oracle is the program's OUTPUT (the accept rows) or a located §22.5
// diagnostic (the refuse rows).

#include <cstdio>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

// A program with the imports the transposed heads live in.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; import System.Threading; "
           "import System.Collections.Generic; "
           "class Item public Name: string; "
           "  public constructor(n: string) this.Name := n; end constructor; "
           "end Item; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

void dump(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

// The §22.5 refusal a name the member does NOT declare must draw.
bool noSuchParameter(const JitResult& r, const char* name) {
    return diagnosticMessageContains(r, "has no parameter named") &&
           diagnosticMessageContains(r, name);
}

// The refusal the pre-fix tree drew at EVERY one of these call sites — the
// one the fix removes.
bool carriesNoNames(const JitResult& r) {
    return diagnosticMessageContains(r, "carries no parameter names");
}

// ---------------------------------------------------------------------------
// The CONSTRUCTORS: `Atomic` / `AtomicInteger` (stdlib §6.4).
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_named_args_registry_7500,
           atomic_constructor_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var a := new Atomic<int>(initial := 5); "
                       "    Console.WriteLine(a.Value.ToString()); ")),
        std::string("5"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           atomicinteger_constructor_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var a := new AtomicInteger<int>(initial := 7); "
                       "    Console.WriteLine(a.Value.ToString()); ")),
        std::string("7"));
}

// ---------------------------------------------------------------------------
// The MEMBERS (stdlib §4.7, §4.11, §4.17, §4.18, §6.1, §6.2, §6.4, §6.10).
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_named_args_registry_7500,
           atomic_exchange_binds_the_name_it_declares) {
    // stdlib §6.4: "Exchange(value) atomically replaces `Value` with `value`
    // and returns the previous value."
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var a := new Atomic<int>(5); "
                       "    Console.WriteLine(a.Exchange(value := 9).ToString()); "
                       "    Console.WriteLine(a.Value.ToString()); ")),
        std::string("5\n9"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           atomicinteger_add_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var a := new AtomicInteger<int>(7); "
                       "    Console.WriteLine(a.Add(delta := 3).ToString()); "
                       "    Console.WriteLine(a.Value.ToString()); ")),
        std::string("10\n10"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           stack_push_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new Stack<int>(); "
                       "    s.Push(item := 11); "
                       "    Console.WriteLine(s.Peek().ToString()); ")),
        std::string("11"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           queue_enqueue_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var q := new Queue<int>(); "
                       "    q.Enqueue(item := 13); "
                       "    Console.WriteLine(q.Peek().ToString()); ")),
        std::string("13"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           sortedset_add_and_contains_bind_the_names_they_declare) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new SortedSet<int>(); "
                       "    s.Add(item := 17); "
                       "    Console.WriteLine(s.Contains(item := 17).ToString()); ")),
        std::string("True"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           sorteddictionary_containskey_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var d := new SortedDictionary<int, string>(); "
                       "    d.Add(1, \"x\"); "
                       "    Console.WriteLine(d.ContainsKey(key := 1).ToString()); ")),
        std::string("True"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           owningstack_push_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new OwningStack<Item>(); "
                       "    s.Push(item := new Item(\"a\")); "
                       "    Console.WriteLine(s.Peek().Name); ")),
        std::string("a"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           owningqueue_enqueue_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var q := new OwningQueue<Item>(); "
                       "    q.Enqueue(item := new Item(\"b\")); "
                       "    Console.WriteLine(q.Peek().Name); ")),
        std::string("b"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           owninglinkedlist_addlast_binds_the_name_it_declares) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var l := new OwningLinkedList<Item>(); "
                       "    l.AddLast(item := new Item(\"c\")); "
                       "    using var it := l.PopFirst(); "
                       "    Console.WriteLine(it.Name); ")),
        std::string("c"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           mutex_tryacquire_binds_the_name_it_declares) {
    JitResult r = runAot(prog("    using var m := new Mutex(); "
                               "    using var l := m.TryAcquire(timeout := "
                               "TimeSpan.FromSeconds(1)); "
                               "    Console.WriteLine(\"ok\"); "));
    if (carriesNoNames(r)) dump(r);
    BEZEL_EXPECT_FALSE(carriesNoNames(r));
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           cancellationtokensource_cancelafter_binds_the_name_it_declares) {
    JitResult r = runAot(prog("    using var c := new CancellationTokenSource(); "
                               "    c.CancelAfter(delay := TimeSpan.FromSeconds(60)); "
                               "    Console.WriteLine(\"ok\"); "));
    if (carriesNoNames(r)) dump(r);
    BEZEL_EXPECT_FALSE(carriesNoNames(r));
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           taskscope_cancelafter_binds_the_name_it_declares) {
    // A `TaskScope` may only be consumed by `await using` (stdlib §6.10), so
    // the row is pinned by the ABSENCE of the §22.5 refusal — the shape this
    // test is about — and not by the scope's own disposal diagnostic.
    JitResult r = runAot(prog("    using var t := new TaskScope(); "
                               "    t.CancelAfter(delay := TimeSpan.FromSeconds(60)); "));
    if (carriesNoNames(r)) dump(r);
    BEZEL_EXPECT_FALSE(carriesNoNames(r));
}

// ---------------------------------------------------------------------------
// A name the member does NOT declare is REFUSED (§22.5) — the table's names
// are now a GATE, not a decoration.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_named_args_registry_7500,
           stack_push_with_an_undeclared_name_is_refused) {
    JitResult r = runAot(prog("    using var s := new Stack<int>(); "
                               "    s.Push(x := 5); "));
    if (!noSuchParameter(r, "`x`")) dump(r);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(noSuchParameter(r, "`x`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.5"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           atomic_exchange_with_an_undeclared_name_is_refused) {
    JitResult r = runAot(prog("    using var a := new Atomic<int>(5); "
                               "    a.Exchange(v := 6); "));
    if (!noSuchParameter(r, "`v`")) dump(r);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(noSuchParameter(r, "`v`"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           sortedset_add_with_an_undeclared_name_is_refused) {
    JitResult r = runAot(prog("    using var s := new SortedSet<int>(); "
                               "    s.Add(x := 5); "));
    if (!noSuchParameter(r, "`x`")) dump(r);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(noSuchParameter(r, "`x`"));
}

BEZEL_TEST(builtin_named_args_registry_7500,
           atomic_constructor_with_an_undeclared_name_is_refused) {
    JitResult r = runAot(prog("    using var a := new Atomic<int>(v := 5); "));
    if (!noSuchParameter(r, "`v`")) dump(r);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(noSuchParameter(r, "`v`"));
}

// ---------------------------------------------------------------------------
// CONTROLS: the POSITIONAL spellings are unchanged, and the heads the table
// does NOT carry names for keep their loud refusal.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_named_args_registry_7500,
           positional_spellings_are_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var a := new Atomic<int>(5); "
                       "    Console.WriteLine(a.Value.ToString()); "
                       "    using var s := new Stack<int>(); "
                       "    s.Push(11); "
                       "    Console.WriteLine(s.Peek().ToString()); "
                       "    using var q := new Queue<int>(); "
                       "    q.Enqueue(13); "
                       "    Console.WriteLine(q.Peek().ToString()); ")),
        std::string("5\n11\n13"));
}

// `TaskCompletionSource` (stdlib §6.6) declares BOTH a generic form
// (`SetResult(result: T)`) and a non-generic one (`SetResult()`), so the head
// cannot answer which one a name binds to; the row is deliberately NOT
// transcribed and the loud refusal stands.
BEZEL_TEST(builtin_named_args_registry_7500,
           an_untranscribed_head_keeps_its_loud_refusal) {
    JitResult r = runAot(prog("    using var t := new TaskCompletionSource<int>(); "
                               "    t.SetResult(result := 5); "));
    if (!carriesNoNames(r)) dump(r);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(carriesNoNames(r));
}

}  // namespace
