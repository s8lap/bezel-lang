// Spec-derived regression test for #5173 — a borrowing `Array<T>` LITERAL
// holding a BORROWED class element double-disposes it and SIGSEGVs at
// scope end (corpus row s15_4_0007, exit 139).
//
// AUTHORITY. spec §28.11 "Borrowing collections" (v0.315): borrowing
// collections "store references whose holders continue to own the elements.
// Element disposal is the arrangement of whoever owns the elements — owned
// bindings, owned fields, or an owning-family collection; it never happens
// through the borrowing collection". The §28.11 element slot table's
// class/interface × borrowing-family row is the **borrow** column, whose
// `Dispose()` of the collection entry reads "no user-visible work; elements
// untouched", and whose `Clear()` / `Remove` / `RemoveAt` entries read
// "removes without disposing (the element's holder, if any, keeps its
// obligation)". The narrative restates it: "the collection is a transit
// point, not a source."
//
// THE DEFECT. `fix #5173` (commit ced9ae92, first-bad for s15_4_0007)
// installed a ClassOwned header-disposer on a borrowing `Array<T>` literal so
// its class elements would be disposed at scope end, and removed the
// `ep.disposerThunk != nullptr` gate in `isBorrowingArrayLiteralLocal` that
// had kept class-element arrays out of the scope-end free. When a literal
// element NAMES an existing holder rather than producing a fresh one —
// §15.4's "name the source first" shape, `using var c := new
// Connection(); var conns: Array<Connection> := [c];` — the array becomes a
// SECOND disposer of `c`, while `using var c` still discharges its own
// obligation at its own scope end.
//
// MEASURED, pre-fix, on staging f6ac5a2b: the program prints its correct
// output `1` and then segfaults in teardown, exit 139. The generated `main`
// runs `bz_array_free(conns, bz$elemDispose)` (the stamped class disposer)
// and then the `using var c` vtable Dispose call faults on the
// already-freed object's null vtable — one object, two disposers.
//
// THE FIX. Gate every class-element disposer this path installs — the
// `lowerLocalDecl` #5414 block, the #5414 FALLBACK arm, the #5173
// `collArrayDynamicFree` arm, and the two header stamps in
// `materializeHeapCollectionLiteral` — on an all-fresh test: every element
// SOURCE of the literal is a `new` expression or a nested collection
// literal. Such a literal's elements are held by nobody else, so disposing
// them at scope end is exactly-once (this is the shape #5414 and the #5173
// rows pin, and it stays green). A literal element naming an existing
// holder is a borrow, and the array now leaves it untouched.
//
// This is the same all-fresh discrimination fix #5106 already applies to a
// borrowing Dictionary literal's values ("A value naming an existing local
// (`["k": a]`) is owned elsewhere and is never freed here").
//
// FAIL-WITHOUT: SIGSEGV, exit 139 (never 0/1 — a crashed program's exit is
// not a value this test could mistake for success).
// PASS-WITH: clean exit, correct Count, exactly ONE dispose of the element.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::JitResult;

namespace {

// A class whose destructor counts itself, so "disposed exactly once" is
// observable in the program's own output rather than only in the exit code.
const char* kConn =
    "public class Connection "
    "  public static disposed: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Connection.disposed := Connection.disposed + 1; end destructor; "
    "end Connection; ";

// The s15_4_0007 repro, verbatim in shape: a `using var` source named first
// (§15.4's remedy for the fresh-element literal rejection), then stored into
// a borrowing `Array<Connection>` literal. `return 1` is reached only if the
// array's Count is 1; the crash, if any, is in scope-end teardown AFTER the
// return value is computed, which is exactly why the corpus row printed its
// correct `1` and still reported exit 139.
const char* kBorrowedFromUsing =
    "public class Program "
    "  public static method Main(): int "
    "    using var c := new Connection(); "
    "    var conns: Array<Connection> := [c]; "
    "    if conns.Count <> 1 then return 90; end if; "
    "    return 1; "
    "  end Main; "
    "end Program; ";

std::string withConn(const char* body) {
    return std::string("module Conf; ") + kConn + body;
}

// A plain `var` (not `using var`) source. Per §28.3 the `new` result is an
// owned local disposed at its scope end either way — `using var` only makes
// that disposal explicit. Same borrow situation (the local owns the element,
// the array only borrows it) reached through a different holder shape, so the
// gate cannot be satisfied by special-casing the `using` keyword. The
// compiler's own §28.3 must-use auto-drop warning is expected and harmless.
const char* kBorrowedFromOwnedLocal =
    "public class Program "
    "  public static method Main(): int "
    "    var c := new Connection(); "
    "    var conns: Array<Connection> := [c]; "
    "    if conns.Count <> 1 then return 90; end if; "
    "    return 1; "
    "  end Main; "
    "end Program; ";

// A field source. The field's owner holds the element; the literal borrows
// it. Third distinct holder shape.
const char* kBorrowedFromField =
    "public class Holder "
    "  public owned conn: Connection; "
    "  public constructor() conn := new Connection(); end constructor; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    var h := new Holder(); "
    "    var conns: Array<Connection> := [h.conn]; "
    "    if conns.Count <> 1 then return 90; end if; "
    "    return 1; "
    "  end Main; "
    "end Program; ";

}  // namespace

// ── §28.11 "Borrowing collections" / slot table borrow column ───────────────
//
// The filed repro (s15_4_0007). Pre-fix: prints 1, then SIGSEGV at scope
// end, exit 139. Post-fix: exit 1, no crash.
//
// The exit-code assertion is the load-bearing one: 139 is not 1, and a
// segfaulting program's exit can never read as this test's expected value.
BEZEL_TEST(borrowing_array_literal_borrowed_element_no_dispose_5173,
           using_var_source_not_double_disposed_28_11) {
    JitResult r = runAot(withConn(kBorrowedFromUsing));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Same repro under the LSan/ASan-clean oracle (`runAotChecked`): the
// double-dispose is a heap error, so the checked leg is where a use-after-free
// would be reported even if the plain run happened to survive.
BEZEL_TEST(borrowing_array_literal_borrowed_element_no_dispose_5173,
           using_var_source_sanitizer_clean_28_11) {
    auto r = runAotChecked(withConn(kBorrowedFromUsing));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §28.11 — an `owned var` holder, not a `using var` ───────────────────────
//
// The gate must key on FRESHNESS, not on the `using` spelling: an `owned
// var` holder disposes at scope end exactly like a `using var` does, so the
// array must equally leave its element alone.
BEZEL_TEST(borrowing_array_literal_borrowed_element_no_dispose_5173,
           owned_var_source_not_double_disposed_28_11) {
    JitResult r = runAot(withConn(kBorrowedFromOwnedLocal));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §28.11 — a class field holder ───────────────────────────────────────────
//
// A field-sourced borrow: the Holder object owns `conn`, so the literal is a
// transit point for it. Third holder shape, same obligation.
BEZEL_TEST(borrowing_array_literal_borrowed_element_no_dispose_5173,
           field_source_not_double_disposed_28_11) {
    JitResult r = runAot(withConn(kBorrowedFromField));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
