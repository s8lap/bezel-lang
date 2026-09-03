// Spec-derived regression tests for issue #3799 — the §28.11 collection-store
// (`.Add` / `.Insert` / `.Push` / `.Enqueue` / `.AddRange`) and indexer-set
// escape legs were not driven from the lambda-body pass; a borrow of an owned
// local stored into an ESCAPING collection from inside a lambda body compiled
// clean and heap-use-after-freed, while the byte-identical METHOD-level
// spelling has been rejected since #1539/#1886.
//
// THE RULE, quoted from the spec (v0.286):
//
//   §28.11 *Borrowing collections*, "Storing a borrow": "parameter-,
//   receiver-, and field-sourced borrows are admitted; a borrow of a
//   **scope-bound local** stored into a collection that provably outlives
//   that scope — the collection is a field, is reachable from one, escapes
//   the method, or is a local of strictly wider scope — is **rejected** at
//   the store site."
//
// THE HOLE. The §28.11 channel has three sibling sinks: `checkBorrowStoreEscape`
// for assignments, `checkCollectionBorrowStoreEscape` for storing-method
// CALLS, and `checkIndexerSetBorrowStoreEscape` for the indexer-set. The
// first is driven from `handleAssignment`, which the dataflow reaches
// through `visitStmt`'s `AssignmentStmt` arm; the second from `visitExpr`'s
// `CallExpr` arm; the third from `handleAssignment`. None of `visitStmt` /
// `handleAssignment` / `visitExpr` enters a LAMBDA BODY. The lambda-body
// pre-pass `scanLambdaBodyBorrowStores` (the #3565 driver) drove only the
// first sibling — the other two were unreachable from inside a closure,
// while the byte-identical method-level spellings have been rejected for
// revisions. MEASURED ON THE PARENT (3729220): each compiling rc=0 with
// NO diagnostic and dying by heap-use-after-free under ASan:
//
//   `using var r := new R();`
//   `var mk := lambda(): int => do this.items.Add(r); return 0; end;`
//   `mk();`                                       → rc=0, UAF on `b.items[0]`
//   `var mk := lambda(): int => do this.items.Insert(0, r); return 0; end;`
//   `mk();`                                       → rc=0, UAF
//   `var mk := lambda(): int => do this.items.Push(r); return 0; end;`
//   `mk();`                                       → rc=0, UAF
//   `var mk := lambda(): int => do this.items[0] := r; return 0; end;`
//   `mk();`                                       → rc=0, UAF
//
// FIX SHAPE. `scanLambdaBodyBorrowStores` now drives ALL THREE §28.11
// channels — the existing assignment-channel call (#3565) plus the two
// siblings — for every expression reachable through a lambda body's
// statements. Each sibling additionally accepts `fromLambdaBody`, which
// triggers the SAME enclosing-frame retry `checkBorrowStoreEscape` already
// uses (the #3727/#3744 leg): the body's own seed only knows locals
// DECLARED inside the lambda, so a borrow of an owned local CAPTURED from
// the enclosing method resolves to no source the first time and would
// silently slip past the §28.11 rejection. With the retry installed, the
// enclosing method's owned-obligation facts are temporarily swapped into
// `states_` and the query re-asks the same authority, so a captured owned
// local is correctly identified as the borrow source. The fix never crosses
// into a nested lambda — a nested lambda's own stores are walked when the
// flat lambda list reaches it, with ITS body's seed (the #3656 grammar).
//
// SOUNDNESS DISCIPLINE. Every positive is paired with a structurally similar
// but spec-LEGAL negative the fix must not reject — a control row already
// rejected pre-fix (no-op pin) and a control row where the source is
// `unowned` (admitted) — and the negatives are run through the AOT oracle
// for their VALUE, per §6c.

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The shared prelude. `R` is an ordinary class; the field `items` is a
// borrowing-family collection (stdlib §4.17), whose `Add` / indexer-set are
// the §28.11 channels the regression covers.
constexpr const char* kR =
    "module M; import System; "
    "class R "
    "  private v: int := 5; "
    "  public method V(): int return this.v; end V; "
    "end R; ";

std::string prog(const std::string& body) {
    return std::string(kR) + body;
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Compiles, links, runs; returns captured stdout with trailing newlines
// trimmed. A program that did not run returns a sentinel, so a row that stops
// compiling fails legibly rather than matching an empty string.
std::string runOut(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

}  // namespace

// ============================================================ POSITIVES ----

// POSITIVE (§28.11 *Storing a borrow*, #3799) — the canonical `.Add` shape
// from the issue's own repro: a borrow of a captured `using var` local
// stored into a field-reachable borrowing-family collection from inside a
// block-bodied lambda body. Pre-fix: rc=0, UAF. Post-fix: rejected at the
// store site with the §28.11 "would outlive its source" diagnostic.
BEZEL_TEST(borrow_store_in_lambda_to_collection_3799,
           add_borrowed_captured_in_lambda_rejected_28_11) {
    const std::string p = prog(
        "class Box "
        "  public owned items: Array<R> := new Array<R>(); "
        "  public method Fill(): int "
        "    using var r := new R(); "
        "    var mk := lambda(): int => do "
        "      this.items.Add(r); "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Fill; "
        "  public method ReadFirst(): int return this.items[0].V(); end ReadFirst; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Fill(); "
        "    Console.WriteLine(b.ReadFirst().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `r` passed to `.Add(...)` on `this` would "
                       "outlive its source"));
}

// POSITIVE (§28.11 *Storing a borrow*, #3799) — the `.Insert` spelling of
// the same store. `.Insert` is one of the §28.11 storing-method family the
// fix's driver walks; this row pins that the rejection covers the family,
// not just `.Add` alone.
BEZEL_TEST(borrow_store_in_lambda_to_collection_3799,
           insert_borrowed_captured_in_lambda_rejected_28_11) {
    const std::string p = prog(
        "class Box "
        "  public owned items: Array<R> := new Array<R>(); "
        "  public method Fill(): int "
        "    using var r := new R(); "
        "    var mk := lambda(): int => do "
        "      this.items.Insert(0, r); "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Fill; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Fill(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `r` passed to `.Insert(...)` on `this` "
                       "would outlive its source"));
}

// POSITIVE (§28.11 *Storing a borrow*, #3799) — the `.Push` sibling of the
// §28.11 storing-method family. Same shape, same expected diagnostic stem.
BEZEL_TEST(borrow_store_in_lambda_to_collection_3799,
           push_borrowed_captured_in_lambda_rejected_28_11) {
    const std::string p = prog(
        "class Box "
        "  public owned items: Stack<R> := new Stack<R>(); "
        "  public method Fill(): int "
        "    using var r := new R(); "
        "    var mk := lambda(): int => do "
        "      this.items.Push(r); "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Fill; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Fill(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `r` passed to `.Push(...)` on `this` would "
                       "outlive its source"));
}

// POSITIVE (§28.11 *Storing a borrow*, #3799) — the indexer-set sibling.
// §28.11 lists "Add, Insert, indexer-set" together, so the indexer-set sink
// `coll[i] := value` is a borrow store exactly like `.Add(value)`. This
// pins the third §28.11 channel (the indexer-set half) the fix's driver
// now walks.
BEZEL_TEST(borrow_store_in_lambda_to_collection_3799,
           indexer_set_borrowed_captured_in_lambda_rejected_28_11) {
    const std::string p = prog(
        "class Box "
        "  public owned items: Array<R?> := new Array<R?>(); "
        "  public method Fill(): int "
        "    this.items.Add(null); "
        "    using var r := new R(); "
        "    var mk := lambda(): int => do "
        "      this.items[0] := r; "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Fill; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Fill(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `r` stored into `this[...]` would outlive "
                       "its source"));
}

// POSITIVE (§28.11 *Storing a borrow*, #3799) — the SAME-frame source shape:
// the `using var` is declared INSIDE the lambda body, not captured from the
// enclosing method. The body's own seed already contains it, so this row
// passes WITHOUT the enclosing-frame retry — pinning that the fix's primary
// path works (and the retry is the second-line defence, not the only one).
BEZEL_TEST(borrow_store_in_lambda_to_collection_3799,
           add_borrowed_local_in_lambda_body_rejected_28_11) {
    const std::string p = prog(
        "class Box "
        "  public owned items: Array<R> := new Array<R>(); "
        "  public method Fill(): int "
        "    var mk := lambda(): int => do "
        "      using var r := new R(); "
        "      this.items.Add(r); "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Fill; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Fill(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `r` passed to `.Add(...)` on `this` would "
                       "outlive its source"));
}

// ============================================================ NEGATIVES ----

// NEGATIVE (control, §28.11 *Storing a borrow* — no-op pin) — the
// method-level spelling has been rejected since #1539/#1886, so this row
// is rejected pre-fix and post-fix; it pins that the fix did not BREAK a
// pre-existing rejection.
BEZEL_TEST(borrow_store_in_lambda_to_collection_3799,
           method_level_add_borrowed_rejected_no_op_pin) {
    const std::string p = prog(
        "class Box "
        "  public owned items: Array<R> := new Array<R>(); "
        "  public method Fill(): int "
        "    using var r := new R(); "
        "    this.items.Add(r); "
        "    return 0; "
        "  end Fill; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Fill(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `r` passed to `.Add(...)` on `this` would "
                       "outlive its source"));
}

// NEGATIVE (§28.11 *Storing a borrow*, false-positive guard) — the source
// is a BORROW parameter (§28.11 admits parameter-, receiver-, and field-
// sourced borrows; the scope-bound-LOCAL rejection is what the §28.11
// rule fires on, and a plain borrow parameter is a §28.5 back-reference
// whose caller-side source really does outlive the constructed object).
// The program must compile clean AND run to completion with the right
// exit code. Pairs the positives above to pin that the fix is not "any
// `.Add` inside a lambda body is rejected".
BEZEL_TEST(borrow_store_in_lambda_to_collection_3799,
           add_borrow_param_in_lambda_accepted_28_11) {
    auto r = runAot(prog(
        "class Box "
        "  public owned items: Array<R> := new Array<R>(); "
        "  public method Fill(q: R): int "
        "    var mk := lambda(): int => do "
        "      this.items.Add(q); "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Fill; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    using var q := new R(); "
        "    b.Fill(q); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
