// Spec-derived regression tests for issue #3746 — the OWNED-PARAMETER axis of
// the §28.3 borrow-store escape rule.
//
// THE RULES, quoted from the spec (v0.286):
//
//   §28.3 "Local variables, ownership, and borrows", sub-heading *Captured
//   owned bindings*: "Inside a closure, a captured owned binding — an owned
//   local or `owned` parameter of the enclosing body — is uniformly in
//   **borrow state**".
//
//   §28.3, sub-heading *Owned-local discharge requirement*, "Auto-drop
//   (affine)": "When an owned local — or an `owned` parameter — reaches a
//   normal-exit scope end without having reached a discharge point, and is not
//   in moved-from state, it is **disposed implicitly at that scope end** (for
//   an `owned` parameter, at the end of the method)".
//
//   §28.3 *Borrow lifetime rules*: "A borrow may be stored in a field of an
//   object only if the borrow's source provably outlives that object. … **The
//   admission.** A borrowed scope-bound *local* never satisfies the rule — it
//   is disposed at the end of its scope, which a field of the object outlives
//   … A borrow whose source *does* outlive the object **is** admissible; the
//   idiomatic form binds it through a constructor parameter … where the
//   caller-side source outlives the constructed object."
//
//   §28.11 *Borrowing collections*, "Storing a borrow": "parameter-,
//   receiver-, and field-sourced borrows are admitted; a borrow of a
//   **scope-bound local** stored into a collection that provably outlives that
//   scope … is **rejected** at the store site."
//
// THE HOLE. The admission of a *parameter-sourced* borrow rests on the premise
// the spec states in the same sentence — "the caller-side source outlives the
// constructed object". An `owned` parameter falsifies it: the caller
// transferred ownership away and THIS method disposes the value at its end.
// `ownedLocalBorrowSource` nonetheless filtered its result to a `LocalSymbol`,
// so an `owned` parameter never became a borrow source and four escaping
// destinations accepted a borrow of it. `checkClosureEscapesOnReturn`'s §28.9
// leg — the RETURN channel of the very same escape rule — has always answered
// correctly for owned parameters (it resolves through `states_`, which `run()`
// seeds with them, and carries its own `ParameterSymbol` arm), so this is one
// rule's two channels disagreeing on one axis, not a new rule.
//
// MEASURED ON THE PARENT (f517ddb), each with `p: owned R`, each paired with a
// byte-identical `using var` LOCAL twin that the parent already REJECTS:
//
//   this.h := lambda(): int => p.V();     rc=0, no diagnostic → SIGSEGV
//   g := lambda(): int => p.V();  (out)   rc=0, no diagnostic → SIGSEGV
//   this.r := p;                          rc=0, no diagnostic → garbage int
//   var b := p; this.items.Add(b);        rc=0, no diagnostic → garbage int
//   var b := p; this.items[0] := b;       rc=0, no diagnostic → garbage int
//
// WHAT THIS FIX DELIBERATELY DOES NOT CHANGE, and why it is not this rule's
// business. Issue #3746's own filed repro stores the closure into a
// METHOD-SCOPE LOCAL and then calls `p.Dispose()` before invoking it. That
// program's owned-LOCAL twin behaves identically on the parent (accepted,
// SIGSEGV), so it is not a parameter axis at all; and the consume table's row
// K3.10 marks an explicit `Dispose()` **unfrozen** — a site-announced kill
// "permitted while borrows of the value live: the announcement is the reader's
// notice and the flag is the safety net" — with §28.2 checked disposal mode as
// that net. VERIFIED: under `--checked-disposal` both spellings raise a clean
// `System.DisposedAccessException` instead of faulting. The rows below
// therefore never rely on an explicit `Dispose()`; every positive escapes the
// method activation structurally.
//
// SOUNDNESS DISCIPLINE. Every positive is paired with a structurally similar
// but spec-LEGAL negative the fix must not reject, and the negatives are run
// through the AOT oracle for their VALUE — an accepted program that stopped
// computing the right answer is not an acceptance (§6c). The boundaries that
// matter are (a) the parameter MODIFIER (a plain borrow parameter is the §28.5
// back-reference the spec's admission is written for), (b) the §28.8 transfer
// shapes (a bare read into an `owned` field, and `move`), and (c) the
// DESTINATION — a method-scope local outlives nothing, and is still accepted.

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The shared prelude: `R` is an ordinary class, so `p: owned R` is an owned
// obligation of the callee's frame (§28.3) whose capture is a §28.9 borrow.
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

// The clause every new leg shares, asserted verbatim: if a second spelling of
// "why an `owned` parameter's value does not outlive this call" ever appears,
// these rows fail.
constexpr const char* kParamClause =
    "is an `owned` parameter, whose value THIS method disposes at its end";

// ------------------------------------------------------------ POSITIVES ----

// POSITIVE (§28.3 *Borrow lifetime rules* / §28.9, #3746) — a closure
// capturing an `owned` parameter stored into an INSTANCE FIELD. The field's
// object outlives the call; `p` does not.
BEZEL_TEST(owned_param_store_escape_3746,
           closure_into_instance_field_rejected_28_3) {
    const std::string p = prog(
        "class Box "
        "  public h: (() -> int)?; "
        "  public method Take(p: owned R) "
        "    this.h := lambda(): int => p.V(); "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Take(new R()); "
        "    Console.WriteLine(b.h!().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `p` stored into a field would outlive its "
                       "source"));
    BEZEL_EXPECT_TRUE(rejectsWith(p, kParamClause));
}

// POSITIVE (§28.3 *Borrow lifetime rules*, #3746) — the PLAIN-STORE spelling
// of the same destination, no closure at all: a bare read of the `owned`
// parameter into a NON-`owned` field. (Into an `owned` field it is the §28.8
// transfer and stays accepted — the negative below.)
BEZEL_TEST(owned_param_store_escape_3746,
           plain_store_into_borrow_field_rejected_28_3) {
    const std::string p = prog(
        "class Box "
        "  public r: R?; "
        "  public method Take(p: owned R) "
        "    this.r := p; "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Take(new R()); "
        "    Console.WriteLine(b.r!.V().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `p` stored into a field would outlive its "
                       "source"));
    BEZEL_EXPECT_TRUE(rejectsWith(p, kParamClause));
}

// POSITIVE (§28.3 *Borrow lifetime rules* / §22.3, #3746) — written through an
// OUT parameter, whose location is in the CALLER's frame. That frame outlives
// this call by construction, so it outlives `p`'s disposal.
BEZEL_TEST(owned_param_store_escape_3746,
           closure_through_out_param_rejected_28_3) {
    const std::string p = prog(
        "class C "
        "  public static method Take(p: owned R, out g: () -> int) "
        "    g := lambda(): int => p.V(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Take(new R(), out var g); "
        "    Console.WriteLine(g().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `p` written through an out/ref parameter "
                       "would outlive its source"));
    BEZEL_EXPECT_TRUE(rejectsWith(p, kParamClause));
}

// POSITIVE (§28.11 "Storing a borrow", #3746) — the element store into a
// borrowing-family collection reachable through the receiver. §28.11's
// admission of "parameter-sourced" borrows carries the same premise §28.3's
// field rule does, and an `owned` parameter falsifies it the same way.
BEZEL_TEST(owned_param_store_escape_3746,
           collection_add_on_field_receiver_rejected_28_11) {
    const std::string p = prog(
        "class Box "
        "  public owned items: Array<R> := new Array<R>(); "
        "  public method Take(p: owned R) "
        "    var b := p; "
        "    this.items.Add(b); "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Take(new R()); "
        "    Console.WriteLine(b.items[0].V().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `p` passed to `.Add(...)` on `this` would "
                       "outlive its source"));
    BEZEL_EXPECT_TRUE(rejectsWith(p, kParamClause));
}

// POSITIVE (§28.11 "Storing a borrow", #3746) — the INDEXER-SET spelling of
// the same store. It is a separate leg of the implementation with its own
// diagnostic, so it can regress independently and earns its own row.
BEZEL_TEST(owned_param_store_escape_3746,
           indexer_set_on_field_receiver_rejected_28_11) {
    const std::string p = prog(
        "class Box "
        "  public owned items: Array<R?> := new Array<R?>(); "
        "  public method Take(p: owned R) "
        "    this.items.Add(null); "
        "    var b := p; "
        "    this.items[0] := b; "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Take(new R()); "
        "    Console.WriteLine(b.items[0]!.V().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "borrow of `p` stored into `this[...]` would outlive "
                       "its source"));
    BEZEL_EXPECT_TRUE(rejectsWith(p, kParamClause));
}

// ------------------------------------------------------------ NEGATIVES ----

// NEGATIVE (§28.3) — the DESTINATION boundary. A closure capturing the `owned`
// parameter stored into a METHOD-SCOPE LOCAL is not an escape at all: the
// local dies with the activation that disposes `p`, so no call of it can be
// sequenced after the disposal. This is the row that pins the fix did NOT
// become "any store of a borrow of an owned parameter is rejected" — and it is
// the shape issue #3746 filed, whose owned-LOCAL twin behaves identically.
BEZEL_TEST(owned_param_store_escape_3746,
           closure_into_method_scope_local_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "class C "
        "  public static method Take(p: owned R): int "
        "    var f := lambda(): int => 0; "
        "    f := lambda(): int => p.V(); "
        "    return f() + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new R()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;")), "10");
}

// NEGATIVE (§28.3 *Borrow lifetime rules*, clause 1 — the §28.5
// back-reference) — a PLAIN (borrow) parameter stored into a field. This is
// the idiom the spec's admission is written for: the caller-side source really
// does outlive the constructed object. `symbolIsOwnedObligation` answers false
// for an unmarked parameter, so nothing here moved.
BEZEL_TEST(owned_param_store_escape_3746,
           borrow_param_into_field_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "class Box "
        "  public r: R?; "
        "  public method Take(p: R) "
        "    this.r := p; "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var q := new R(); "
        "    using var b := new Box(); "
        "    b.Take(q); "
        "    Console.WriteLine(b.r!.V().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;")), "5");
}

// NEGATIVE (§28.8, the ownership-TRANSFER pattern) — a bare read of the
// `owned` parameter into an `owned` field. That is a move-on-read into an
// owned slot, not a borrow store; the field's object becomes the owner and
// nothing dangles. `checkBorrowStoreEscape`'s pre-existing `targetIsOwnedField`
// skip runs BEFORE the source query, so widening the query cannot reach it —
// this row proves that ordering held.
BEZEL_TEST(owned_param_store_escape_3746,
           bare_read_into_owned_field_accepted_28_8) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "class Box "
        "  public owned r: R?; "
        "  public method Take(p: owned R) "
        "    this.r := p; "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Take(new R()); "
        "    Console.WriteLine(b.r!.V().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;")), "5");
}

// NEGATIVE (§28.8.1, the marked transfer) — `move p` into an `owned` field.
// `recordMovesIn` leaves `p` MovedFrom before this check runs, so no live
// source resolves and the transfer is admitted, parameter arm or not.
BEZEL_TEST(owned_param_store_escape_3746,
           move_into_owned_field_accepted_28_8) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "class Box "
        "  public owned r: R?; "
        "  public method Take(p: owned R) "
        "    this.r := move p; "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Take(new R()); "
        "    Console.WriteLine(b.r!.V().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;")), "5");
}

// NEGATIVE (§28.11) — the RECEIVER boundary of the collection legs. The
// collection is a method-scope LOCAL, not reachable beyond the method, so
// `RecvKind::Local` applies and the verdict stays the declaring-block depth
// comparison the parameter arm is deliberately kept out of. The collection
// dies with the activation that disposes `p`; nothing escapes.
BEZEL_TEST(owned_param_store_escape_3746,
           collection_add_on_local_receiver_accepted_28_11) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "class C "
        "  public static method Take(p: owned R): int "
        "    using var items := new Array<R>(); "
        "    var b := p; "
        "    items.Add(b); "
        "    return items[0].V(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new R()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;")), "5");
}

// NEGATIVE (§28.3 *Producers*) — a value-typed COPY out of the `owned`
// parameter is not a borrow of it. `n` is an `int`; storing it in a field
// carries no lifetime obligation. `borrowSourceForExpr`'s
// `returnedExprMayBorrow` gate is what keeps this out, and the parameter arm
// sits behind that gate, not in front of it.
BEZEL_TEST(owned_param_store_escape_3746,
           value_copy_into_field_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "class Box "
        "  public n: int := 0; "
        "  public method Take(p: owned R) "
        "    this.n := p.V(); "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.Take(new R()); "
        "    Console.WriteLine(b.n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;")), "5");
}

}  // namespace
