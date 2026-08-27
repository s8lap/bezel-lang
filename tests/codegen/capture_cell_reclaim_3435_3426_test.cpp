// Codegen — issues #3435 and #3426 (§27.5 Closures, §28.9 Runtime-managed
// types, spec v0.283): two independent holes in the reclamation of a boxed
// CAPTURE CELL — the heap allocation §27.5 mandates ("Captured variables are
// heap-allocated in a closure object") and §28.9 makes the implementation's own
// obligation to reclaim ("Function-typed values … carry no disposal obligation"
// — the runtime owes the reclaim instead).
//
// They share a symptom (4 bytes / 1 allocation per execution for an `int`
// capture, 8 for a `string`) and a neighbourhood, and they are DIFFERENT
// MECHANISMS. Neither fix subsumes the other; both are needed.
//
// ── #3435 — a capture whose block was SPLIT loses the scope-end free.
//
//     var total := 0;
//     for var i in 0..2 do total := total + i; end;
//     var f := lambda(a: int): int => a + total;   // 4 B leaked, pre-fix
//
// The lambda does not escape, so `total` IS in cellFreeEligible_ and the
// scope-end `boxedCellFree` should arm. It did not, because `lowerLambda`
// excluded every GUARDED-boxing cell, and `boxLocalSlot` picks guarded vs fresh
// by asking whether a StoreInst to the slot sits in the CURRENT BASIC BLOCK.
// For adjacent statements that is the same block (which is why the plain
// `var t := 0; var f := lambda() => t;` shape was always clean); put ANY
// block-splitting construct between the declaration and the capture and the
// guarded path is taken and the cell was dropped on the floor. The issue's
// discriminator — clean when the assignment moves OUT of the loop — is that
// block boundary, not the assignment, and the `if`-split row below shows the
// same leak with no loop anywhere.
//
// The fix drops the `inlineN > 0` conjunct from the existing `loopStack_.empty()`
// admission: that proof (no backward edge above the capture site ⇒ the guarded
// cell is allocated at most once per invocation) never had anything to do with
// the capture being an inline owned array. The two hazards a wider admission
// could reintroduce were already answered downstream in
// consumeLastLambdaCapturedCells — a disposal that READS THROUGH the cell is
// deferred into the owner's scope (#966 / #1017), and a captured local declared
// in a SHALLOWER scope than the capture site is skipped outright (#969). The
// `do var g := () => x; end; return x;` row below pins that second guard: it
// still leaks 4 B, deliberately, because freeing there would be a UAF.
//
// ── #3426 — a cell shared by one ESCAPING and one NON-ESCAPING lambda.
//
//     var k: int := 7;
//     var g := lambda() => k;      // non-escaping
//     Console.WriteLine(g().ToString());
//     var f := lambda() => k;      // escapes by return
//     return f;                    // 4 B leaked, pre-fix
//
// Two mechanisms could reclaim that cell and this shape falls between them, by
// each one's own correct design:
//   * the SCOPE-END route (cellFreeEligible_) arms only when NO escaping lambda
//     captures the local — `f`'s env holds a raw cell pointer that outlives the
//     frame, so freeing at scope end would be a use-after-free;
//   * the ENVDTOR route (#694) arms only when the escaping closure is the
//     cell's ONLY holder — `g` points at the same cell and #694 can say nothing
//     about its release order.
// Both are individually sound; nothing covered the overlap.
//
// The fix adds the third mechanism, from the observation that the two holder
// classes ARE ordered. Every non-escaping co-capturer is in
// closureReleaseEligible_, so its env is released at a scope end inside this
// frame. The escaping capturer is, by construction of
// definitelyEscapingLambdas_, NOT in closureReleaseEligible_ (`return f` is an
// escape for the ClosureEscapeWalker), so `lowerLocalDecl` registers no
// scope-end release for it at all and its envdtor cannot run until the value
// the frame handed out is dropped by someone else — STRICTLY after the frame is
// gone, on every exit edge. The sole escaping holder is therefore the last
// reader, and freeing the cell in its envdtor (after that envdtor's own content
// release) is the cell's one and only free. `collectCellFreeEligible` now
// records that owner per capture key; `lowerLambda` arms it under
// `loopStack_.empty()`, the same backward-edge proof the captured-PARAM branch
// already used.
//
// ── THE DIRECTION OF DANGER. These walkers arm FREES: under-admitting leaks,
// over-admitting is a use-after-free through a returned closure. Six
// OVER-RELEASE GUARD rows below assert shapes where the cell must NOT be freed.
// TWO of them are PROVEN FALLIBLE — a scratch compiler with the single conjunct
// each one names deleted turns that row red, and only that row:
//
//   field_store_escape_inside_loop_…       loopStack_.empty() removed
//                                          -> "ERROR: AddressSanitizer:
//                                              attempting double-free"
//   argument_passed_lambda_is_not_a_…      definitelyEscapingLambdas_ conjunct
//                                              removed
//                                          -> "ERROR: AddressSanitizer:
//                                              heap-use-after-free"
//
// The other four (two escaping capturers, an escaping capture site inside a
// loop, a nested lambda's widened capture, an outer local read after the
// capturing scope) are HONEST ABOUT WHAT THEY ARE: leak-and-value parity rows,
// not proven-fallible guards. Deleting the `lams.size() != 1` conjunct did NOT
// turn the two-escaper row red, and deleting `loopStack_.empty()` did not turn
// the in-loop-return row red either — measured, not assumed. Both shapes stay
// safe by accident: the escaping closure that is NOT returned on a given path
// is never released at all (a returned-escaping lambda gets no scope-end
// release), so however many closures the shape builds, at most one envdtor ever
// runs and there is nothing to double-free. The two conjuncts remain necessary
// for shapes where those closures ARE released — which is exactly the
// field-store row above — and the four parity rows remain worth their cost as
// leak-count and value assertions on shapes the fix must not disturb.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (`bezelc --sanitize=address`, LeakSanitizer on):
//
//   program                                        pre-fix            post-fix
//   #3435 loop-assigned capture                    4 B / 1 alloc      clean
//   #3435 same, ×500 iterations                    2000 B / 500       clean
//   #3435 if-split capture (no loop), ×500         2000 B / 500       clean
//   #3435 control: no split between decl+capture   clean              clean
//   #3426 int capture, escaping + non-escaping     4 B / 1 alloc      clean
//   #3426 string capture                           8 B / 1 alloc      clean
//   #3426 PARAMETER capture                        4 B / 1 alloc      clean
//   #3426 §21.1 `const`-bound escaping lambda      4 B / 1 alloc      clean
//   #3426 int capture, ×500 calls                  2000 B / 500       clean
//   guard: two escaping capturers                  104 B / 6          104 B / 6
//   guard: escaping capture inside a loop          68 B / 5           68 B / 5
//   guard: nested-lambda widened capture           4 B / 1            4 B / 1
//   guard: outer local read after inner scope      4 B / 1            4 B / 1
//   guard: field-store escape inside a loop        16 B / 4           16 B / 4
//   guard: argument-passed lambda                  4 B / 1            4 B / 1
//
// The six guard rows leak identically before and after by design — that
// equality IS the assertion that the fix admitted nothing extra.
// per §27.5, §28.9 (issues #3435, #3426)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a non-empty return here is itself the leak assertion.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for the guard rows, whose shapes leak by design (a cell no
// mechanism may reclaim). The oracle there is the exact VALUE: an over-eager
// free shows up as a wrong number, a crash, or an ASan abort under `--full`.
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// #3435 — the block-split scope-end capture-cell free
// ═══════════════════════════════════════════════════════════════════════════

// ── The issue's exact repro. A non-escaping lambda captures a local that is
// assigned inside a preceding loop; pre-fix the loop's basic blocks pushed
// boxLocalSlot onto the guarded path and the cell was never freed (4 B / 1).
// The value must also be right: 1 + (0+1+2) = 4.
// per §27.5, §28.9 (issue #3435)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           loop_assigned_capture_cell_freed_at_scope_end_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..2 do "
        "      total := total + i; "
        "    end; "
        "    var f := lambda(a: int): int => a + total; "
        "    Console.WriteLine(f(1).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "4\n");
}

// ── LOOP-AMPLIFIED twin of the row above. A one-shot 4-byte LSan row can pass
// on BOTH sides of a fix when the allocation is still stack-reachable at exit;
// 500 executions of the shape make the pre-fix report 2000 B / 500 allocations,
// which no reachability accident explains. Printed total: 500 × 4 = 2000.
// per §27.5, §28.9 (issue #3435)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           loop_assigned_capture_cell_freed_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Step(n: int): int "
        "    var total := 0; "
        "    for var i in 0..n do "
        "      total := total + i; "
        "    end; "
        "    var f := lambda(a: int): int => a + total; "
        "    return f(1); "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..499 do "
        "      acc := acc + C.Step(2); "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2000\n");
}

// ── THE TRIGGER IS THE BLOCK SPLIT, NOT THE LOOP. The same leak with no loop
// between the declaration and the capture — just an `if`. This row is what
// makes the fix's stated root cause falsifiable: had the mechanism really been
// loop-specific, this program would have been clean pre-fix. It was not
// (2000 B / 500 allocations at ×500). Printed total: 500 × (1 + 5) = 3000.
// per §27.5, §28.9 (issue #3435)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           if_split_capture_cell_freed_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Run(c: bool): int "
        "    var total := 0; "
        "    if c then "
        "      total := 5; "
        "    end; "
        "    var f := lambda(a: int): int => a + total; "
        "    return f(1); "
        "  end Run; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..499 do "
        "      acc := acc + C.Run(true); "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3000\n");
}

// ── PARITY CONTROL: the issue's own discriminator. With nothing between the
// declaration and the capture the store is in the capture site's own basic
// block, boxLocalSlot takes the FRESH path, and the cell was always freed.
// Green before and after; it is here so the pair above reads as a parity
// assertion rather than a bare leak check.
// per §27.5, §28.9 (issue #3435)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           unsplit_capture_cell_freed_parity_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    total := total + 1; "
        "    var f := lambda(a: int): int => a + total; "
        "    Console.WriteLine(f(1).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2\n");
}

// ── OVER-RELEASE GUARD (#969). The captured local is declared in an ENCLOSING
// scope and read AFTER the block that captures it, so its cell must survive
// that block: freeing it at the inner scope's end would leave the later read
// dereferencing freed storage. consumeLastLambdaCapturedCells' declaration-
// depth check declines the free, so this program keeps its deliberate 4-byte
// leak — before AND after the fix — and both values stay right
// (55 + 0 + 1 + 2 = 58). Plain runAot: the leak is intentional here.
// per §27.5, §28.9 (issue #3435)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           outer_local_capture_cell_not_freed_at_inner_scope_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 55; "
        "    for var i in 0..2 do "
        "      x := x + i; "
        "    end; "
        "    do "
        "      var g := lambda() => x; "
        "      Console.WriteLine(\"g=\" + g().ToString()); "
        "    end; "
        "    Console.WriteLine(\"x=\" + x.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "g=58\nx=58\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// #3426 — the cell shared by one escaping and one non-escaping lambda
// ═══════════════════════════════════════════════════════════════════════════

// ── The issue's exact repro (with its `(* … *)` comments corrected to the
// §1.5 forms — see #3438). Pre-fix 4 B / 1 allocation, reclaimed by nobody.
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           shared_cell_escaping_plus_non_escaping_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int "
        "    var k: int := 7; "
        "    var g := lambda() => k; "
        "    Console.WriteLine(\"g=\" + g().ToString()); "
        "    var f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(); "
        "    Console.WriteLine(\"h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "g=7\nh=7\n");
}

// ── The STRING axis of the same shape (the issue's 8-byte variant). It is the
// interesting one for ordering: unlike an `int` capture, BOTH envdtors here
// perform a content release THROUGH the cell (the first capture inherits the
// producer's suppressed +1, the second mints its own retain). The new free sits
// after this envdtor's own release, and the non-escaping sibling's envdtor has
// already run — at its scope end, inside the frame — by the time the escaping
// closure can be dropped at all. A wrong ordering here is an over-release, not
// a leak, so the value assertion matters as much as the LSan verdict.
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           shared_cell_string_capture_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(p: string): () -> string "
        "    var k: string := p + \"cd\"; "
        "    var g := lambda() => k; "
        "    Console.WriteLine(\"g=\" + g()); "
        "    var f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(\"ab\"); "
        "    Console.WriteLine(\"h=\" + h()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "g=abcd\nh=abcd\n");
}

// ── The PARAMETER axis. A captured parameter's cell is keyed by Symbol*, not
// by an AST decl node, and lowerLambda handles it in a separate branch; the
// same overlap leaked there identically (4 B / 1). Both branches now consult
// the one shared owner map, rather than growing a second spelling of the
// predicate.
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           shared_cell_parameter_capture_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(k: int): () -> int "
        "    var g := lambda() => k; "
        "    Console.WriteLine(\"g=\" + g().ToString()); "
        "    var f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(7); "
        "    Console.WriteLine(\"h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "g=7\nh=7\n");
}

// ── The §21.1 runtime-`const` spelling of the escaping lambda. #3402 made
// LocalConstDecl visible to collectCellFreeEligible and
// collectDefinitelyEscapingLambdas; this row confirms the new third route sees
// it too, so the `const` and `var` spellings reclaim identically.
// per §21.1, §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           shared_cell_const_bound_escaping_lambda_freed_spec_21_1_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int "
        "    var k: int := 7; "
        "    var g := lambda() => k; "
        "    Console.WriteLine(\"g=\" + g().ToString()); "
        "    const f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(); "
        "    Console.WriteLine(\"h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "g=7\nh=7\n");
}

// ── LOOP-AMPLIFIED twin: 500 calls, each producing one shared cell that the
// returned closure's release must reclaim. Pre-fix 2000 B / 500 allocations.
// Printed total: 500 × 3 = 1500.
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           shared_cell_freed_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(u: int): () -> int "
        "    var k: int := u; "
        "    var g := lambda() => k; "
        "    var t: int := g(); "
        "    var f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..499 do "
        "      var h := C.Make(3); "
        "      acc := acc + h(); "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1500\n");
}

// ── OVER-RELEASE GUARD: TWO escaping capturers. Both `f` and `g` are returned
// on some path, so both are definitely-escaping and neither is provably the
// last holder of `k`'s cell — the walker records no owner and the cell is
// deliberately left to leak (104 B / 6 allocations, identical before and
// after). If a widening ever armed BOTH envdtors this becomes a double free;
// the values are the tripwire (7+1 and 7+2).
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           two_escaping_capturers_cell_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Make(sel: bool): () -> int "
        "    var k: int := 7; "
        "    var f := lambda() => k + 1; "
        "    var g := lambda() => k + 2; "
        "    if sel then return f; end; "
        "    return g; "
        "  end Make; "
        "  public static method Main(): int "
        "    var a := C.Make(true); "
        "    var b := C.Make(false); "
        "    Console.WriteLine(\"a=\" + a().ToString()); "
        "    Console.WriteLine(\"b=\" + b().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "a=8\nb=9\n");
}

// ── PARITY ROW (leak-count/value), not a proven guard. The escaping capture
// site is INSIDE A LOOP: one syntactic lambda node there makes one closure PER
// ITERATION over a single shared cell, and `singleCaptureLocals_` cannot see
// that (it counts lambda NODES). `loopStack_.empty()` declines it.
//
// ⚠ This row does NOT prove that conjunct necessary. MEASURED on a scratch
// build with `loopStack_.empty()` deleted from both #3426 arms: values stay
// right (g=7 / h=8) and it does not abort — 68 B/5 becomes 64 B/4. It is safe
// by accident, because a returned-escaping lambda gets no scope-end release,
// so at most one envdtor ever runs. The row that DOES prove the conjunct is
// `field_store_escape_inside_loop`, where deleting it yields a real
// `AddressSanitizer: attempting double-free`.
//
// Kept as a parity row: the leak count and the values must not move.
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           escaping_capture_inside_loop_cell_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int "
        "    var k: int := 7; "
        "    var g := lambda() => k; "
        "    Console.WriteLine(\"g=\" + g().ToString()); "
        "    for var i in 0..2 do "
        "      var f := lambda() => k + i; "
        "      if i = 1 then return f; end; "
        "    end; "
        "    return lambda() => 0; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(); "
        "    Console.WriteLine(\"h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "g=7\nh=8\n");
}

// ── OVER-RELEASE GUARD: a NESTED lambda's widened capture. §27.5/#1135 pushes
// every symbol reached through a lambda nested inside another lambda's body
// into the escaping set, with no owner node recorded — the inner lambda may
// itself escape the outer one, and the two share the grandparent's cell. The
// new route must decline such a key rather than pick the one owner it happens
// to have seen, so this shape keeps its 4-byte leak, unchanged by the fix.
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           nested_lambda_widened_capture_cell_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int "
        "    var k: int := 7; "
        "    var g := lambda() => (lambda() => k)(); "
        "    Console.WriteLine(\"g=\" + g().ToString()); "
        "    var f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(); "
        "    Console.WriteLine(\"h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "g=7\nh=7\n");
}

// ── OVER-RELEASE GUARD, PROVEN FALLIBLE (`loopStack_.empty()`). A field store
// is a §27.5/#694 definite escape when the field is assigned EXACTLY ONCE in
// the method — counted SYNTACTICALLY. Put that one assignment inside a loop and
// it executes N times, each store dropping the previous closure, whose envdtor
// runs immediately: N releases of closures that all share ONE guarded cell.
// `loopStack_.empty()` is the conjunct that declines it. Measured on a scratch
// compiler with that conjunct deleted, this exact program dies with
// "ERROR: AddressSanitizer: attempting double-free"; with it, the cell is left
// to leak (16 B / 4 allocations, identical before and after the fix) and both
// values are right — s = 7, h = 7 + 2.
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           field_store_escape_inside_loop_cell_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  private _h: () -> int := lambda() => 0; "
        "  public method Fill(n: int): int "
        "    var k: int := 7; "
        "    var e := lambda() => k; "
        "    var s: int := e(); "
        "    for var i in 0..n do "
        "      this._h := lambda() => k + i; "
        "    end; "
        "    return s; "
        "  end Fill; "
        "  public method Call(): int "
        "    return this._h(); "
        "  end Call; "
        "end C; "
        "class D "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    Console.WriteLine(\"s=\" + c.Fill(2).ToString()); "
        "    Console.WriteLine(\"h=\" + c.Call().ToString()); "
        "    return 0; "
        "  end Main; "
        "end D;"),
        "s=7\nh=9\n");
}

// ── OVER-RELEASE GUARD, PROVEN FALLIBLE (the definitelyEscapingLambdas_
// conjunct). A lambda LITERAL passed as a call argument is CONSERVATIVELY
// escaping — it is not in closureReleaseEligible_, so it lands in the escaping
// set — but it is NOT definitely-escaping: codegen releases the argument
// closure right after the call, INSIDE the frame, so its envdtor is not the
// last reader of the cell at all. Everything after that call still reads `k`
// through it. Requiring definitelyEscapingLambdas_ is what declines this;
// measured on a scratch compiler with that conjunct deleted, this program dies
// with "ERROR: AddressSanitizer: heap-use-after-free". With it, the cell is
// left to leak (4 B / 1 allocation, identical before and after the fix) and
// every value is right.
// per §27.5, §28.9 (issue #3426)
BEZEL_TEST(capture_cell_reclaim_3435_3426,
           argument_passed_lambda_is_not_a_cell_free_owner_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Use(fn: () -> int): int "
        "    return fn(); "
        "  end Use; "
        "  public static method Main(): int "
        "    var k: int := 7; "
        "    var e := lambda() => k; "
        "    Console.WriteLine(\"e=\" + e().ToString()); "
        "    var n: int := C.Use(lambda() => k + 1); "
        "    Console.WriteLine(\"n=\" + n.ToString()); "
        "    Console.WriteLine(\"k=\" + k.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "e=7\nn=8\nk=7\n");
}
