// Spec-derived regression tests for issue #6311 — an unbound owned temporary of
// a §31.4 BUILT-IN EXCEPTION class, passed as a NON-`owned` (borrow)
// argument to a method or constructor, was never disposed and leaked.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Assign.cpp, `buildOrderedArgs`, the
// argument-temporary registration block). §28.3 *Unbound owned temporaries*
// makes a fresh owned producer that is used only by borrowing operations —
// "as a non-`owned` argument" is one of the enumerated ones — an unbound
// owned temporary disposed at the end of the full expression. Codegen already
// registers exactly that disposal for a fresh `new T(...)` / `move` /
// `owned`-returning call argument, but only after passing
//
//     at->kind == Named && at->declaration &&
//     at->declaration->kind == ast::TypeDeclKind::Class
//
// and a built-in exception's `TypeRef` has `declaration == nullptr` — there is
// no user `TypeSymbol` for a language-built-in type (§31.4). So the gate
// answered "not a class" for every `new IOException("a")` /
// `new Exception("a")` argument, NO disposal was registered, and the object
// was owned by nothing: the §28.3 boundary pass reclaimed it nowhere.
//
// §28.3 states the obligation for this population exactly as for any other
// class, and gives the shape almost verbatim under *A callee that stores a
// plain argument*: "in `holder.Hold(new Item());`, where `Hold(x: Item)`
// assigns `x` to a field, the `Item` is disposed at the end of the statement."
// K8.4 is the counterpart: the callee may store it, the temporary is NOT
// lifetime-extended, and `h._e` dangles from the boundary on — which is why
// the rows below read the stored back-reference only INSIDE the same full
// expression, never after it. Reading it later is the spec's recorded hazard
// (§28.3's K4.3 residual trust; §28.2's `DisposedAccessException` backstop),
// not a defect to pin.
//
// THE FIX. The gate now also admits `isBuiltinExceptionTypeRef(at)` — the
// named recognizer the owned-field / using-var auto-dispose paths already use
// for this same population. `setupBuiltinExceptions` mints every §31.4
// hierarchy member a real vtable whose slot 2 is `bz$exDisposeMsg`, which is
// precisely what the `dynamicDispose` this block registers dispatches through.
// The predicate is deliberately narrow: a declaration-less Named TypeRef whose
// astNode NAME resolves in `builtinVtable_` — no more. A vtable-less built-in
// (a collection, a `Task`, a `FileStream`) is not in `builtinVtable_` and stays
// excluded, which is what keeps the word-0-as-vtable read away from a plain C
// struct.
//
// MEASURED fail-without (on 389ca74b, this tree with the fix reverted; LSan
// `detect_leaks=1:exitcode=99:abort_on_error=0`, the recipe `runAotChecked`
// runs): each of the two fail-without rows below printed
// `SUMMARY: AddressSanitizer: 20000 byte(s) leaked in 500 allocation(s).` and
// exited 99, which the harness reports as `ran == false` -> `-999`. With the
// fix both are LSan-clean and return their spec-derived value.
//
// Spec: §28.3 *Unbound owned temporaries*, §28.3 *A callee that stores a plain
// argument*, §28.3 K8.1 / K8.4 (the full-expression boundary), §31.4 (the
// built-in exception hierarchy). Revision v0.338.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value. `expectRanWith` is the ONLY runAotChecked call
// site in this file, and it does not absorb a toolchain skip: a skip makes the
// harness return `ran = false` (aot_detail::markInfra), which the
// `BEZEL_EXPECT_TRUE(r.ran)` below turns into a row failure rather than a
// silent pass.
// The leak-oracle rows must assert BOTH halves: `ran` says the program ran to
// normal termination under ASan/LSan (no leak, no memory error), and the exit
// code says it computed the spec-derived value. Either half alone is
// satisfiable vacuously — a `didNotCrash`-only row passes on a program that
// computes the wrong answer, and a value-only row can be satisfied by a
// sentinel.
void expectRanWith(const std::string& src, int expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

// §28.3 / §31.4 (#6311) — the plain method-parameter shape. `q.Nop(...)`
// declares `Nop(x: Exception)`: a non-`owned`, non-`scoped` class-typed
// parameter, so the argument position is a borrowing use whatever the callee's
// body does with it. The `new IOException("a")` there is a fresh owned
// producer with no name and no other owner; before the fix it leaked 40 B per
// call.
//
// 500 iterations x 1 = 500; 500 mod 251 = 249.
// Pre-fix: LSan `20000 byte(s) leaked in 500 allocation(s)`, exit 99 -> -999.
// Post-fix: LSan-clean, 249.
BEZEL_TEST(codegen_builtin_exception_temp_arg_disposal_6311,
           plain_method_param_IOException_temp_spec_28_3) {
    std::string src =
        "module M; "
        "class Q "
        "  public method Nop(x: Exception): int return 1; end Nop; "
        "end Q; "
        "class P public static method Main(): int "
        "  var q := new Q(); "
        "  var t := 0; "
        "  for var i in 0..<500 do t := t + q.Nop(new IOException(\"a\")); end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §28.3 *A callee that stores a plain argument* / §31.4 (#6311) — the issue's
// own repro shape, and the one §28.3 spells out. `Hr`'s constructor takes a
// plain `e: Exception` and stores it into the non-`owned` back-reference field
// `_e`; the caller passes a fresh `new IOException(...)`. Per K8.4 the callee
// may store it, but the temporary is disposed at the boundary all the same.
//
// `G()` reads `_e.Message` INSIDE the same full expression as the
// construction, which is the sound window (§28.3's own rule: "a temporary
// meets the obligation only when the object dies within the same full
// expression"). Reading `h._e` from a LATER statement is the spec's recorded
// dangling hazard, not this fix's subject — so this row does not do it.
//
// 500 iterations x 5 = 2500; 2500 mod 251 = 241.
// Pre-fix: LSan `20000 byte(s) leaked in 500 allocation(s)`, exit 99 -> -999.
// Post-fix: LSan-clean, 241.
BEZEL_TEST(codegen_builtin_exception_temp_arg_disposal_6311,
           ctor_backref_field_IOException_temp_spec_28_3) {
    std::string src =
        "module M; "
        "class Hr "
        "  public constructor(e: Exception) _e := e; end constructor; "
        "  public readonly _e: Exception; "
        "  public method G(): int return _e.Message.Length; end G; "
        "end Hr; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Hr(new IOException(\"abcde\")); "
        "    t := t + h.G(); "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 241);
}

// §31.4 (#6311) — the ROOT of the hierarchy, not a derived member. The fix
// admits `isBuiltinExceptionTypeRef`, which resolves the NAME against
// `builtinVtable_`; `Exception` is that table's first row and has a different
// `directBuiltinExceptionBase` chain from `IOException`'s, so a fix that had
// only taught itself the derived names would keep this row red.
//
// It also reads the argument back INSIDE the callee (`x.Message.Length`), so it
// pins that the disposal is established at the boundary and not before the
// call — an over-eager dispose-before-call would read a nulled Message slot.
//
// 500 iterations x 3 = 1500; 1500 mod 251 = 245.
// Pre-fix: leaked 500 allocations -> -999. Post-fix: 245.
BEZEL_TEST(codegen_builtin_exception_temp_arg_disposal_6311,
           root_Exception_head_read_inside_callee_spec_31_4) {
    std::string src =
        "module M; "
        "class Q "
        "  public method Nop(x: Exception): int return x.Message.Length; end Nop; "
        "end Q; "
        "class P public static method Main(): int "
        "  var q := new Q(); "
        "  var t := 0; "
        "  for var i in 0..<500 do t := t + q.Nop(new Exception(\"abc\")); end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §28.3 *A callee that stores a plain argument* (#6311) — the INLINE chain
// spelling, where the whole construction-and-read is one full expression and
// the temporary therefore meets its obligation (§28.3: "a temporary meets the
// obligation only when the object dies within the same full expression"). This
// is the row that keeps the fix honest in the other direction: the disposal is
// at the boundary, so the `_e.Message` read the same expression performs must
// still see a live object.
//
// 500 iterations x 5 = 2500; 2500 mod 251 = 241.
BEZEL_TEST(codegen_builtin_exception_temp_arg_disposal_6311,
           inline_ctor_chain_read_within_full_expression_spec_28_3) {
    std::string src =
        "module M; "
        "class Hr "
        "  public constructor(e: Exception) _e := e; end constructor; "
        "  public readonly _e: Exception; "
        "  public method G(): int return _e.Message.Length; end G; "
        "end Hr; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    t := t + new Hr(new IOException(\"abcde\")).G(); "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 241);
}

// ACCEPT CONTROL — the OVER-RELEASE guard, and the most important row here.
// A leak fix that disposes too much is a use-after-free, which is worse than
// the bug it replaced.
//
// Here the argument is a BORROW: a `using var` local, which §28.10 binds to
// its own scope and §28.3 makes an ordinary named owned value. It is NOT an
// unbound temporary (nothing produced it inside an expression), so the
// argument block must leave it alone — the local's own scope-end disposal is
// the single correct site, and disposing it at the call would free it while
// the scope still holds it.
//
// The row reads `e.Message` AFTER the call, which is exactly the read an
// over-broad dispose corrupts. MEASURED that this read is not vacuous: the
// same shape with an explicit `e.Dispose()` before it reports
// `ERROR: AddressSanitizer: SEGV ... in main` (the `bz$exDisposeMsg` slot-null
// leaves `Message` at 0x8) and exits 99 -> -999.
//
// This row is GREEN on the pre-fix tree too, and must STAY green: it passed
// before the fix and passes after it, so it pins the direction the fix must not
// move in. 500 iterations x (5 + 5) = 5000; 5000 mod 251 = 231.
BEZEL_TEST(codegen_builtin_exception_temp_arg_disposal_6311,
           accept_control_borrow_local_arg_not_disposed_spec_28_10) {
    std::string src =
        "module M; "
        "class Q "
        "  public method Nop(x: Exception): int return x.Message.Length; end Nop; "
        "end Q; "
        "class P public static method Main(): int "
        "  var q := new Q(); "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    using var e := new IOException(\"abcde\"); "
        "    t := t + q.Nop(e) + e.Message.Length; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 231);
}

// ACCEPT CONTROL — the USER-CLASS twin. A fresh `new Res()` at the same plain
// parameter position has a real `TypeSymbol` of kind Class, so it was already
// registered before this fix; this row was green on the pre-fix tree and must
// stay green. It is the discriminator that proves the fix ADDED the built-in
// exception population rather than changing what a class-kind argument does —
// if the widened predicate had also disturbed the class-kind arm (a dropped
// `dynamicDispose`, a scope moved), this row goes red.
//
// 500 iterations x 7 = 3500; 3500 mod 251 = 237.
BEZEL_TEST(codegen_builtin_exception_temp_arg_disposal_6311,
           accept_control_user_class_temp_still_disposed_spec_28_3) {
    std::string src =
        "module M; "
        "class Res "
        "  public N: int; "
        "  public constructor() N := 7; end constructor; "
        "end Res; "
        "class Q "
        "  public method Nop(x: Res): int return x.N; end Nop; "
        "end Q; "
        "class P public static method Main(): int "
        "  var q := new Q(); "
        "  var t := 0; "
        "  for var i in 0..<500 do t := t + q.Nop(new Res()); end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 237);
}

}  // namespace