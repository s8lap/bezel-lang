// Ownership — issue #9182: a `Dictionary` LOCAL stored into another
// `Dictionary`'s VALUE slot (`_d[k] := inner`) must be REJECTED at the store
// site, exactly as the user-class twin already is.
//
// SPEC (v0.359).
//   §28.11 *Borrowing collections* → *Storing a borrow*, clause 2:
//     "a borrow of a **scope-bound binding** — an `owned` parameter included,
//     its value being disposed at the method's end (§28.3) — stored into a
//     collection that provably outlives that scope — the collection is a
//     field, is reachable from one, is held by a parameter that is not
//     `owned` or reached through one …, escapes the method, or is a local of
//     strictly wider scope — **is rejected at the store site**;"
//   §28.11 *The element slot policy*: "class / interface type | **borrows** |
//     owned" — a borrowing family's slot for a class-typed `V` is a BORROW
//     slot, and its `Dispose()` row is "no user-visible work; elements
//     untouched" — the collection does NOT keep the value alive.
//   §28.3 *Producers* (K1.5): a fresh `new` production bound to a local is an
//     OWNED atom; §28.9 *Trivial-dispose classes* gives a trivial-dispose
//     instance a compiler-inserted free at its holder's release point —
//     "scope end for locals and `using var`" — so the local's backing is
//     reclaimed AT THE LOCAL'S OWN SCOPE END.
//
// THE DEFECT (MEASURED on r30-c1 @ d3f56e723, the issue's own program):
// the store compiled clean with NO diagnostic, and the slot then read freed
// memory —
//   ==ERROR: AddressSanitizer: heap-use-after-free
//     READ of size 1 … in bz_key_eq (src/runtime/bezel_runtime_collections.cpp:1503)
//     … in bz_dict_try_get_str … in bz$M.C$Get … in main
//   freed by thread T0 here: … in bz_dict_free (…:1690) … in main
//
// WHY THE STORE RULE DID NOT SEE IT. The store sinks' borrow-source lookup
// asks `borrowSourceForExpr`, whose borrow-vs-copy gate
// (`returnedExprMayBorrow` → `isBorrowableType`) answers FALSE for every
// DECLARATION-LESS BUILTIN CLASS — `Dictionary`, `Array`, `Set`, … — because
// it classifies on `declaration->kind` and a builtin has no user `TypeDecl`
// (#7825 records the same mis-classification and adds
// `isBorrowableOwnedSinkType` for it). So the scope-bound owned local was
// never seen as a borrow source and §28.11's clause-2 rejection never fired.
// The USER-CLASS twin (`Dictionary<string, Res>` over a user class `Res`) was
// already rejected — these rows restore that shape parity.
//
// THE ORACLES. Each rejection row names the specific §28.11 store-rule
// diagnostic. The control row (a container that does NOT outlive the source)
// must still compile and run clean under the strict oracle, printing the
// value the slot holds — the row that a plausible over-broad fix (rejecting
// every builtin-collection store) would turn red.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAotChecked;

namespace {

// The §28.11 store-rule diagnostic's stable fragments: the rule's own
// "would outlive its source" wording and its section citation.
constexpr const char* kOutlives = "would outlive its source";
constexpr const char* kRule = "§28.11 borrowing-collection store rule";

void expectRejected(const bezel::test::JitResult& r, const char* needle) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

// The issue's own program, verbatim, with the read-back driven 200 times so
// the base arm's use-after-free is deterministic rather than incidental.
std::string issueProgram() {
    return "module M; "
           "public class C "
           "  private owned _d: Dictionary<string, Dictionary<string, int>> := "
           "      new Dictionary<string, Dictionary<string, int>>(); "
           "  public method Put(k: string) "
           "    var inner := new Dictionary<string, int>(); "
           "    inner[\"x\"] := 1; "
           "    _d[k] := inner; "
           "  end Put; "
           "  public method Get(k: string): int "
           "    var inner := _d[k]; "
           "    return inner[\"x\"]; "
           "  end Get; "
           "end C; "
           "public class Program "
           "  public static method Main() "
           "    var i := 0; "
           "    while i < 200 do "
           "      using var c := new C(); "
           "      c.Put(\"a\"); "
           "      Console.WriteLine(c.Get(\"a\")); "
           "      i := i + 1; "
           "    end while; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ===========================================================================
// (1) THE ISSUE'S OWN PROGRAM — the indexer-set store of a scope-bound
//     `Dictionary` local into a FIELD-held dictionary's VALUE slot.
// ===========================================================================

BEZEL_TEST(dictionary_value_slot_borrow_store_9182,
           spec_28_11_clause_2_issue_repro_value_slot_store_rejected) {
    auto r = runAotChecked(issueProgram());
    expectRejected(r, kOutlives);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRule));
    // The diagnostic must name the SOURCE (the scope-bound binding whose
    // value dies at its scope end) and the DESTINATION (the collection that
    // outlives it) — a rejection that named neither would pin nothing.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`inner`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`_d`"));
}

// ===========================================================================
// (2) THE `.Add` TWIN — the same store through the storing-method call.
// ===========================================================================

BEZEL_TEST(dictionary_value_slot_borrow_store_9182,
           spec_28_11_clause_2_add_call_twin_value_slot_store_rejected) {
    auto r = runAotChecked(
        "module M; "
        "public class C "
        "  private owned _d: Dictionary<string, Dictionary<string, int>> := "
        "      new Dictionary<string, Dictionary<string, int>>(); "
        "  public method Put(k: string) "
        "    var inner := new Dictionary<string, int>(); "
        "    inner[\"x\"] := 1; "
        "    _d.Add(k, inner); "
        "  end Put; "
        "end C; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new C(); "
        "    c.Put(\"a\"); "
        "  end Main; "
        "end Program;");
    expectRejected(r, kOutlives);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRule));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`inner`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`_d`"));
}

// ===========================================================================
// (3) THE GENERIC TWIN — the same store inside a user GENERIC class, whose
//     VALUE slot is `Dictionary<string, T>` at `T = Dictionary<string, int>`.
// ===========================================================================

BEZEL_TEST(dictionary_value_slot_borrow_store_9182,
           spec_28_11_clause_2_generic_holder_value_slot_store_rejected) {
    auto r = runAotChecked(
        "module M; "
        "public class Box<T> "
        "  private owned _d: Dictionary<string, T> := "
        "      new Dictionary<string, T>(); "
        "  public method Put(k: string) "
        "    var inner := new Dictionary<string, int>(); "
        "    inner[\"x\"] := 1; "
        "    _d[k] := inner; "
        "  end Put; "
        "end Box; "
        "public class Program "
        "  public static method Main() "
        "    using var b := new Box<Dictionary<string, int>>(); "
        "    b.Put(\"a\"); "
        "  end Main; "
        "end Program;");
    expectRejected(r, kOutlives);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRule));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`inner`"));
}

// ===========================================================================
// (4) THE CONTROL — the container is a LOCAL of the SAME scope, so it does
//     NOT outlive the source's scope and §28.11's clause 2 does not apply.
//     The store is admitted, the slot holds the value, and the program reads
//     it back. A plausible over-broad fix — rejecting every store of a
//     builtin-collection local into a borrowing collection — turns this row
//     red; so does a fix that rejects the same-scope store by mistake.
// ===========================================================================

BEZEL_TEST(dictionary_value_slot_borrow_store_9182,
           spec_28_11_clause_2_same_scope_local_store_accepted) {
    auto r = runAotChecked(
        "module M; "
        "public class Program "
        "  public static method Main() "
        "    var outer := new Dictionary<string, Dictionary<string, int>>(); "
        "    var inner := new Dictionary<string, int>(); "
        "    inner[\"x\"] := 1; "
        "    outer[\"k\"] := inner; "
        "    Console.WriteLine(outer[\"k\"][\"x\"]); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kOutlives));
}
