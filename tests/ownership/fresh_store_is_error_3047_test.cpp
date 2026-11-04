// Spec-derived regression for issue #3047, lint A — the §15.4 FRESH-STORE rule
// is a compile ERROR (proposal 0207, v0.346).
//
// §15.4 *Collection literals* (v0.355), the store-site counterpart:
//
//   "a fresh unbound owned temporary of class type (§28.3.11) — a `new`
//    expression, a collection literal, an `owned`-returning call result, a
//    `move` result, or a **`Task`-family call result** (calls mint, §28.3) —
//    in a single-element store position of a borrowing-family collection is a
//    compile error at the store site. The positions are the single-element
//    stores of §28.11's value-copy shape (ii) — `Add`, `Insert`, `Push`,
//    `Enqueue`, `TryAdd`, `LinkedList<T>`'s `AddFirst`/`AddLast`/`AddBefore`/
//    `AddAfter`, `GetOrAdd`'s key, `Counter<K>.Increment`'s key — and the
//    indexer-set, **key and value positions alike**, … — and the **key**
//    position of `OwningDictionary<K, V>`"
//
// and §28.11 *Borrowing collections*, the store-site check, clause 4: "the
// store is rejected at the store site by §15.4's fresh-store rule"; K8.4: "a
// fresh unbound owned temporary there is §15.4's fresh-store error";
// Appendix C.3 states the same invariant.
//
// Before this fix the compiler emitted the v0.243 lint's WARNING for exactly
// this population and accepted the program, so the slot dangled from the end
// of the statement: the #8033 lane MEASURED `s.Push(new Cell())` into a
// borrowing `Stack<Cell>` inside `Mk()` followed by `Mk().Pop().v` as a
// heap-use-after-free under ASan. Every reject row below FAILS on the pre-fix
// compiler (it reports a Warning, not an Error) and passes with the fix.
//
// NOT pinned here, recorded residuals of #3047's proposal-0207 checklist:
//   * the collection-literal fresh form — a literal in a store position does
//     not lower and sema leaves it untyped (#3393), so no program reaches it;
//   * `Counter<K>.Increment`'s key — `Counter<K>` is unimplemented (#3848);
//   * a receiver typed at a borrowing INTERFACE (`IList<T>`, `ICollection<T>`,
//     a derived interface, a constrained type parameter) — #7141.
//
// // per §15.4 (v0.355), §28.11 *Borrowing collections* clause 4, K8.4,
//    Appendix C.3; proposal 0207

#include <iostream>
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// `Item` is trivial-dispose; `Res` carries a destructor (non-trivial-dispose).
// `Pt` is a record (value-typed, outside the rule).
const char* kPreamble =
    "module M; "
    "record Pt(X: int, Y: int); "
    "class Item "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; "
    "class Res "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Console.WriteLine(\"dtor\"); end destructor; "
    "end Res; "
    "class F "
    "  public static method Make(v: int): owned Item return new Item(v); end Make; "
    "  public static async method WorkAsync(): Task<int> return 1; end WorkAsync; "
    "end F; ";

const char* kNeedle = "§15.4 fresh-store error";

int freshStoreErrors(const OwnershipResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kNeedle) == std::string::npos) continue;
        ++n;
    }
    return n;
}

int errorCount(const OwnershipResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

void dumpErrors(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error)
            std::cout << "      error: " << d.message.substr(0, 200) << "\n";
}

// A fresh form: the statements that must precede the store (the `move` form
// needs a named source to move from) and the argument expression itself.
struct Fresh {
    const char* label;
    const char* prelude;
    const char* expr;
};

// The class-typed fresh forms §15.4 names, at an `Item` element. (The literal
// form is #3393's residual; the Task family is exercised at a `Task<int>`
// element below.)
const std::vector<Fresh> kItemFresh = {
    {"new", "", "new Item(1)"},
    {"owned-returning call", "", "F.Make(1)"},
    {"move", "var m := new Item(1); ", "move m"},
};

// The Task-family fresh forms: a stdlib Task-returning call and an `async`
// method call (calls mint, §28.3).
const std::vector<Fresh> kTaskFresh = {
    {"Task.FromResult", "", "Task.FromResult(1)"},
    {"async call", "", "F.WorkAsync()"},
};

// A store position: the receiver's declaration and any setup, and the store
// statement with `@` standing for the fresh argument.
struct Position {
    const char* label;
    const char* setup;
    const char* store;
};

std::string substitute(const char* store, const char* expr) {
    std::string s(store);
    const auto at = s.find('@');
    if (at != std::string::npos) s.replace(at, 1, expr);
    return s;
}

// Compiles `setup; fresh.prelude; store;` inside a method that takes a
// borrowed `t: Item` (a parameter-sourced element the setup may store) and
// asserts EXACTLY one §15.4 fresh-store error and no other error.
bool rejectsOnce(const Position& p, const Fresh& f) {
    const std::string body =
        "class C public static method M(t: Item): int " + std::string(p.setup) +
        std::string(f.prelude) + substitute(p.store, f.expr) +
        " return 0; end M; end C;";
    OwnershipResult r = parseAndCheckOwnership(std::string(kPreamble) + body);
    const bool ok = freshStoreErrors(r) == 1 && errorCount(r) == 1;
    if (!ok) {
        std::cout << "    row FAILED: position `" << p.label << "` × fresh `"
                  << f.label << "`: " << freshStoreErrors(r)
                  << " fresh-store error(s), " << errorCount(r)
                  << " error(s) in total\n";
        dumpErrors(r);
    }
    return ok;
}

bool admitted(const char* label, const std::string& body) {
    OwnershipResult r = parseAndCheckOwnership(std::string(kPreamble) + body);
    const bool ok = errorCount(r) == 0 &&
                    freshStoreErrors(r) == 0;
    if (!ok) {
        std::cout << "    control FAILED: `" << label << "` was rejected\n";
        dumpErrors(r);
    }
    return ok;
}

}  // namespace

// ── Reject rows: every §28.11 (ii) single-element store × every fresh form ──

// Element (value) positions on the borrowing family, at a class element.
BEZEL_TEST(fresh_store_is_error_3047, value_positions_x_class_fresh_forms_spec_15_4) {
    const std::vector<Position> positions = {
        {"Array.Add", "using var a := new Array<Item>(); ", "a.Add(@);"},
        {"Array.Insert", "using var a := new Array<Item>(); ", "a.Insert(0, @);"},
        {"Array indexer-set", "using var a := new Array<Item>(); a.Add(t); ",
         "a[0] := @;"},
        {"LinkedList.AddFirst", "using var l := new LinkedList<Item>(); ",
         "l.AddFirst(@);"},
        {"LinkedList.AddLast", "using var l := new LinkedList<Item>(); ",
         "l.AddLast(@);"},
        {"LinkedList.AddBefore",
         "using var l := new LinkedList<Item>(); var n := l.AddFirst(t); ",
         "l.AddBefore(n, @);"},
        {"LinkedList.AddAfter",
         "using var l := new LinkedList<Item>(); var n := l.AddFirst(t); ",
         "l.AddAfter(n, @);"},
        {"Set.Add", "using var s := new Set<Item>(); ", "s.Add(@);"},
        {"Set.TryAdd", "using var s := new Set<Item>(); ", "var b := s.TryAdd(@);"},
        {"Stack.Push", "using var s := new Stack<Item>(); ", "s.Push(@);"},
        {"Queue.Enqueue", "using var q := new Queue<Item>(); ", "q.Enqueue(@);"},
        {"Dictionary.Add value", "using var d := new Dictionary<int, Item>(); ",
         "d.Add(1, @);"},
        {"Dictionary.TryAdd value", "using var d := new Dictionary<int, Item>(); ",
         "var b := d.TryAdd(1, @);"},
        {"Dictionary indexer-set value",
         "using var d := new Dictionary<int, Item>(); ", "d[1] := @;"},
        {"SortedDictionary.Add value",
         "using var d := new SortedDictionary<int, Item>(); ", "d.Add(1, @);"},
    };
    for (const auto& p : positions)
        for (const auto& f : kItemFresh) BEZEL_EXPECT_TRUE(rejectsOnce(p, f));
}

// Key positions, "key and value positions alike" — on `Dictionary` and on
// `OwningDictionary`, "which owns its values and not its keys".
BEZEL_TEST(fresh_store_is_error_3047, key_positions_x_class_fresh_forms_spec_15_4) {
    const std::vector<Position> positions = {
        {"Dictionary.Add key", "using var d := new Dictionary<Item, int>(); ",
         "d.Add(@, 1);"},
        {"Dictionary.TryAdd key", "using var d := new Dictionary<Item, int>(); ",
         "var b := d.TryAdd(@, 1);"},
        {"Dictionary indexer-set key",
         "using var d := new Dictionary<Item, int>(); ", "d[@] := 1;"},
        {"Dictionary.GetOrAdd key", "using var d := new Dictionary<Item, int>(); ",
         "var v := d.GetOrAdd(@, lambda(k: Item) => 1);"},
        {"OwningDictionary.Add key",
         "using var d := new OwningDictionary<Item, Res>(); ",
         "d.Add(@, new Res(1));"},
        {"OwningDictionary.TryAdd key",
         "using var d := new OwningDictionary<Item, Res>(); ",
         "var b := d.TryAdd(@, new Res(1));"},
        {"OwningDictionary indexer-set key",
         "using var d := new OwningDictionary<Item, Res>(); ",
         "d[@] := new Res(1);"},
    };
    for (const auto& p : positions)
        for (const auto& f : kItemFresh) BEZEL_EXPECT_TRUE(rejectsOnce(p, f));
}

// The Task family: "for a `Task`-family argument the task's disposal (the
// operation would be *abandoned*…)".
BEZEL_TEST(fresh_store_is_error_3047, task_family_fresh_forms_spec_15_4) {
    const std::vector<Position> positions = {
        {"Array<Task>.Add", "using var a := new Array<Task<int>>(); ", "a.Add(@);"},
        {"Array<Task>.Insert", "using var a := new Array<Task<int>>(); ",
         "a.Insert(0, @);"},
        {"Stack<Task>.Push", "using var s := new Stack<Task<int>>(); ", "s.Push(@);"},
        {"Queue<Task>.Enqueue", "using var q := new Queue<Task<int>>(); ",
         "q.Enqueue(@);"},
        {"Dictionary<int, Task>.Add value",
         "using var d := new Dictionary<int, Task<int>>(); ", "d.Add(1, @);"},
    };
    for (const auto& p : positions)
        for (const auto& f : kTaskFresh) BEZEL_EXPECT_TRUE(rejectsOnce(p, f));
}

// The diagnostic keeps the lint's content: the K8 boundary, the hazard by
// family, the two remedies (one for an owning map's key).
BEZEL_TEST(fresh_store_is_error_3047, diagnostic_names_boundary_family_and_remedies_spec_15_4) {
    auto text = [](const std::string& body) {
        OwnershipResult r = parseAndCheckOwnership(std::string(kPreamble) + body);
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error &&
                d.message.find(kNeedle) != std::string::npos)
                return d.message;
        return std::string();
    };
    const std::string triv = text(
        "class C public static method M(): int using var a := new Array<Item>(); "
        "a.Add(new Item(1)); return 0; end M; end C;");
    BEZEL_EXPECT_TRUE(triv.find("K8 full-expression boundary") != std::string::npos);
    BEZEL_EXPECT_TRUE(triv.find("named holder") != std::string::npos);
    BEZEL_EXPECT_TRUE(triv.find("OwningArray") != std::string::npos);
    BEZEL_EXPECT_TRUE(triv.find("a freed instance") != std::string::npos);
    BEZEL_EXPECT_TRUE(triv.find("warning") == std::string::npos);

    const std::string nont = text(
        "class C public static method M(): int using var a := new Array<Res>(); "
        "a.Add(new Res(1)); return 0; end M; end C;");
    BEZEL_EXPECT_TRUE(nont.find("a disposed instance") != std::string::npos);

    const std::string task = text(
        "class C public static method M(): int using var a := new Array<Task<int>>(); "
        "a.Add(Task.FromResult(1)); return 0; end M; end C;");
    BEZEL_EXPECT_TRUE(task.find("an abandoned task") != std::string::npos);

    const std::string odKey = text(
        "class C public static method M(): int "
        "using var d := new OwningDictionary<Item, Res>(); "
        "d[new Item(1)] := new Res(1); return 0; end M; end C;");
    BEZEL_EXPECT_FALSE(odKey.empty());
    BEZEL_EXPECT_TRUE(odKey.find("use the owning family") == std::string::npos);
}

// ── Must-not-fire controls: the population is unchanged ──

BEZEL_TEST(fresh_store_is_error_3047, controls_outside_the_population_are_admitted_spec_15_4) {
    // §15.4's first remedy: "bind the value to a named holder first".
    BEZEL_EXPECT_TRUE(admitted(
        "bound local",
        "class C public static method M(): int "
        "  using var x := new Item(1); using var a := new Array<Item>(); "
        "  a.Add(x); return a.Count; end M; end C;"));
    // The second remedy: "use the owning family, whose store positions take
    // ownership".
    BEZEL_EXPECT_TRUE(admitted(
        "owning collection",
        "class C public static method M(): int "
        "  using var a := new OwningArray<Item>(); a.Add(new Item(1)); "
        "  using var s := new OwningStack<Res>(); s.Push(new Res(2)); "
        "  using var t := new OwningArray<Task<int>>(); t.Add(Task.FromResult(1)); "
        "  return a.Count; end M; end C;"));
    // The owning map's VALUE position takes ownership; only its key is
    // borrowing.
    BEZEL_EXPECT_TRUE(admitted(
        "owning map value",
        "class C public static method M(): int "
        "  using var k := new Item(1); "
        "  using var d := new OwningDictionary<Item, Res>(); "
        "  d.Add(k, new Res(1)); return d.Count; end M; end C;"));
    // A value type and a runtime-managed element carry no classification.
    BEZEL_EXPECT_TRUE(admitted(
        "value / string elements",
        "class C public static method M(p: string): int "
        "  using var a := new Array<Pt>(); a.Add(new Pt(1, 2)); "
        "  using var b := new Array<int>(); b.Add(5); "
        "  using var c := new Array<string>(); c.Add(p + \"x\"); "
        "  return a.Count; end M; end C;"));
    // "a plain parameter of a **user-declared** member — `sink.Consume(new
    // X())`, a user class's own `Add` … — keeps §28.3.11's unbound-temporary
    // admission".
    BEZEL_EXPECT_TRUE(admitted(
        "user-declared sink",
        "class Bag "
        "  public method Add(i: Item): int return i.V; end Add; "
        "  public method Consume(i: Item): int return i.V; end Consume; "
        "end Bag; "
        "class C public static method M(): int "
        "  using var b := new Bag(); "
        "  var x := b.Add(new Item(1)); var y := b.Consume(new Item(2)); "
        "  return x + y; end M; end C;"));
    // The index of a non-dictionary head selects a slot; it is not a store
    // position (no error from the bound value either).
    BEZEL_EXPECT_TRUE(admitted(
        "array index of a bound value",
        "class C public static method M(t: Item): int "
        "  using var a := new Array<Item>(); a.Add(t); a[0] := t; "
        "  return a.Count; end M; end C;"));
}

// ── End to end ──

// The #8033 lane's MEASURED heap-use-after-free: `s.Push(new Cell())` into a
// borrowing `Stack<Cell>`, read through `Mk().Pop().v`. The store inside `Mk`
// is now rejected at compile time with the §15.4 error, so the dangling slot
// can no longer be built.
BEZEL_TEST(fresh_store_is_error_3047, issue_8033_mk_push_fresh_cell_is_rejected_spec_15_4) {
    auto r = runAot(
        "module M; "
        "class Cell public v: int := 7; end Cell; "
        "class C "
        "  public static method Mk(): owned Stack<Cell> "
        "    var s := new Stack<Cell>(); "
        "    s.Push(new Cell()); "
        "    return s; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do t += Mk().Pop().v; end for; "
        "    Console.WriteLine(t); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kNeedle));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "`.Push(...)` store"));
}

// The remedy compiles and runs: the element bound to a named holder whose
// scope covers every read of the slot.
BEZEL_TEST(fresh_store_is_error_3047, named_holder_remedy_compiles_and_runs_spec_15_4) {
    auto r = runAot(
        "module M; "
        "class Cell public v: int := 7; end Cell; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      using var c := new Cell(); "
        "      using var s := new Stack<Cell>(); "
        "      s.Push(c); "
        "      t += s.Pop().v; "
        "    end for; "
        "    Console.WriteLine(t); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("700") != std::string::npos);
}
