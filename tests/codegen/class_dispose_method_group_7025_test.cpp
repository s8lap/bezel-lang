// fix #7025 (spec §8.7 / §27.6 / §28.2, v0.340): the bare method-group
// REFERENCE to a CLASS's own compiler-generated `Dispose` — `var f := d.Dispose;`
// — degraded to a zero-initialized local, and the call was a silent no-op.
//
// THE DEFECT. §8.7: "The compiler generates `IDisposable` on every non-static
// class." The generated `Dispose` has NO MethodDecl and NO MethodSymbol —
// §28.2: "the name is the compiler's: the generated `Dispose()` … cannot be
// overridden", and "Declaring a member named `Dispose` on a class or a record
// is a compile error at that declaration". There is therefore no declaration
// to bind, so sema left the member-access expression symbol-less and
// codegen's §27.6 method-reference path (`CG60`, LlvmBackend_Exprs.cpp) fell
// through to `ret nullptr`. The local then degraded to a zero-initialized
// value (the #2434 warning) and CALLING the method group was a SILENT NO-OP
// (the local's type was never a function type, so the call degraded too) —
// or, with a `() -> ()` annotation, a SIGSEGV on the null cell.
//
// THE RULE. §27.6 (v0.340): "the member resolved on the value exactly as the
// call `v.Scaled()` is". The CALL form `d.Dispose()` is ACCEPTED and works
// (§28.2: the generated `Dispose()` is what `using var`, the owned-field
// cascade, the discard and an explicit call all reach), so the reference form
// must be accepted too — and it must produce a real callable. A
// zero-initialized slot is neither.
//
// WHAT THE FORMED VALUE DOES. §28.2: "An explicit `Dispose()` or direct
// `DisposeAsync()` performs **logical disposal only** — it sets the flag and
// runs the class's cleanup — and does not reclaim its receiver: the receiver's
// block stays live until the holder's release point … a repeated call is the
// no-op stated above in both modes." The vtable dispatch (slot 2) is exactly
// that operation — it does NOT free the receiver's block; the FREE belongs to
// the holder's release point (§28.2's "The compiler-inserted free … the free
// there is logical disposal followed by reclamation"). So the formed value and
// the receiver's own scope-end drop agree on ONE disposal: the method group's
// invocation performs the LOGICAL half (the destructor body, the owned-field
// cascade, the idempotent flag), the holder's release point performs the
// RECLAIM, and §28.2's idempotence makes a second invocation a no-op.
//
// The rows below assert the OBSERVABLE behaviour (the destructor runs exactly
// once, at the right time) — not merely the absence of a crash. The
// pre-fix behaviour (accepted, degraded, the call a no-op / SIGSEGV) fails
// every one of them.
//
// NOT IN SCOPE. The `Dispose` method group over an OWNING COLLECTION
// INTERFACE head (`IOwningCollection<T>` / `IOwningList<T>`) is a DIFFERENT
// shape: the collection's disposal path FREES the collection (it is a
// builtin, not a vtable-bearing class), so a value-capturing group would
// dispose it twice. What such a group MEANS is the open spec-question #7067.
// The rows below pin that the interface receiver is untouched by this fix.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

}  // namespace

// The issue's own repro shape (§8.7 / §27.6): a method group naming a class's
// own `Dispose`, formed and then CALLED. The destructor must run — exactly
// once — when the group is invoked.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_27_6_class_dispose_group_call_runs_the_destructor) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class D "
            "  public constructor() end constructor; "
            "  destructor Console.WriteLine(\"disposed\"); end destructor; "
            "end D; "
            "class C public static method Main(): int "
            "  var d := new D(); "
            "  var f := d.Dispose; "
            "  f(); "
            "  Console.WriteLine(\"after-call\"); "
            "  return 0; "
            "end Main; end C;"),
        "disposed\nafter-call");
}

// The SAME formation with an explicit `() -> ()` annotation — the spelling
// that made the pre-fix build SIGSEGV on the null cell.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_27_6_typed_class_dispose_group_call_runs_the_destructor) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class D "
            "  public constructor() end constructor; "
            "  destructor Console.WriteLine(\"disposed\"); end destructor; "
            "end D; "
            "class C public static method Main(): int "
            "  var d := new D(); "
            "  var f: () -> () := d.Dispose; "
            "  f(); "
            "  Console.WriteLine(\"after-call\"); "
            "  return 0; "
            "end Main; end C;"),
        "disposed\nafter-call");
}

// A group that is NEVER called must leave the receiver's own scope-end
// disposal to do the work — the disposal still happens, exactly once. §28.2:
// the free is the holder's release point's job.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_27_6_uninvoked_class_dispose_group_leaves_the_scope_end_disposal) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class D "
            "  public constructor() end constructor; "
            "  destructor Console.WriteLine(\"disposed\"); end destructor; "
            "end D; "
            "class C public static method Main(): int "
            "  var d := new D(); "
            "  var f := d.Dispose; "
            "  Console.WriteLine(\"never-called\"); "
            "  return 0; "
            "end Main; end C;"),
        "never-called\ndisposed");
}

// Invoking the group TWICE must be a no-op the second time — §28.2:
// "`Dispose()` on any class instance is idempotent: the first call performs
// the disposal; subsequent calls are no-ops." The destructor must print ONCE.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_invoking_the_group_twice_disposes_exactly_once) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class D "
            "  public constructor() end constructor; "
            "  destructor Console.WriteLine(\"disposed\"); end destructor; "
            "end D; "
            "class C public static method Main(): int "
            "  var d := new D(); "
            "  var f := d.Dispose; "
            "  f(); "
            "  f(); "
            "  Console.WriteLine(\"after-calls\"); "
            "  return 0; "
            "end Main; end C;"),
        "disposed\nafter-calls");
}

// The MEMORY oracle: the closure cell the group is backed by is released at
// scope end (§28.9) and the receiver is freed exactly once — no leak, no
// double free. `runAotChecked` returns ran=false on any ASan / LSan fire.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_9_class_dispose_group_is_memory_clean) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var f := d.Dispose; "
        "  f(); "
        "  Console.WriteLine(\"after-call\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL — the CALL form of the same member is untouched: it still disposes
// the receiver and still nulls the slot (§28.2's exactly-once obligation).
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_class_dispose_call_form_still_works) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class D "
            "  public constructor() end constructor; "
            "  destructor Console.WriteLine(\"disposed\"); end destructor; "
            "end D; "
            "class C public static method Main(): int "
            "  var d := new D(); "
            "  d.Dispose(); "
            "  Console.WriteLine(\"after-call\"); "
            "  return 0; "
            "end Main; end C;"),
        "disposed\nafter-call");
}

// CONTROL — an ordinary §27.6 method reference on a USER class is untouched:
// the new arm is limited to the compiler-generated `Dispose`.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_27_6_user_class_method_reference_still_works) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "public class K public method N(): int return 42; end N; end K; "
            "class C public static method Main(): int "
            "  var k: K := new K(); "
            "  var f: () -> int := k.N; "
            "  Console.WriteLine(f()); "
            "  return 0; "
            "end Main; end C;"),
        "42");
}

// CONTROL — the `Dispose` method group over an OWNING COLLECTION INTERFACE
// head is NOT touched by this fix: that receiver's disposal path FREES the
// collection (it is a builtin, not a vtable-bearing class), so what such a
// group means — and its exactly-once obligation — is the open spec-question
// #7067. This row pins that the new arm did not widen onto the interface
// receiver: the program still COMPILES and RUNS (the fix added no rejection
// there). (`runAot` discards the compiler's diagnostics on a successful
// compile, so the verdict is pinned by the run, not by diagnostic text.)
BEZEL_TEST(class_dispose_method_group_7025,
           spec_27_6_owning_collection_interface_dispose_group_is_unchanged) {
    auto r = runAot(
        "module M; "
        "import System; "
        "public class Item public method Get(): int return 4; end Get; end Item; "
        "class C public static method Main(): int "
        "  var oc: IOwningCollection<Item> := new OwningArray<Item>(); "
        "  var f: () -> () := oc.Dispose; "
        "  Console.WriteLine(\"compiled\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// fix #7025 (follow-up, §21.1 / §27.6 / §28.2, v0.340): the three defects the
// non-author review of the two #7025/#7014 commits found.
//
// F1 — THE GROUP'S CAPTURE MUST SURVIVE THE RECEIVER'S DISPOSAL.
// §27.6: "the instance is captured at formation — by reference for class- or
// interface-typed receivers". §28.2: "An explicit `Dispose()` or direct
// `DisposeAsync()` performs **logical disposal only** — it sets the flag and
// runs the class's cleanup — and does not reclaim its receiver: the receiver's
// block stays live until the holder's release point". The pre-fix build
// RECLAIMED the receiver at the explicit call, so the group's later
// invocation read a freed block — ASan "heap-use-after-free … READ of size 8"
// in `disposethunk$Dispose`.
//
// F2 — THE §21.1 `const` SPELLING. A §21.1 runtime `const` local's
// declaration is an `ast::LocalConstDecl` (initializer field `value`), not an
// `ast::LocalDecl` (field `initializer`), so the binding-local scan missed it
// and the group call was a SILENT NO-OP.
//
// F3 — A USER ANNOTATION MUST NOT BE OVERWRITTEN. §27.6: "The reference's
// signature must be compatible with the target function type: its parameter and
// return types unify with the target's under the standard implicit-conversion
// rules (§5.1)." The pre-fix build replaced the annotation unconditionally,
// so a loud base failure became a silent wrong answer.
// ---------------------------------------------------------------------------

// F1: the group is formed, the receiver is explicitly disposed, and the group
// is invoked — the invocation must read a LIVE block (the §28.2 logical
// disposal only), and the program must run to completion under the sanitizer.
// The pre-fix build died with "heap-use-after-free … READ of size 8".
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_group_invoked_after_an_explicit_dispose_is_memory_clean) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var f := d.Dispose; "
        "  d.Dispose(); "
        "  f(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // stdoutText is the RAW captured stdout (the trailing newline is
    // Console.WriteLine's terminator) — the local idiom in this area.
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n");
}

// F1 (the ordinary §27.6 reference twin): the SAME shape over an ordinary
// method reference captures its receiver the same way (§27.6), so the same
// §28.2 rule applies. The method here READS the receiver, so the pre-fix
// build's reclaim made the read a use-after-free — "heap-use-after-free …
// READ of size 8". The review's control was clean only because its method
// returned a constant and never dereferenced the receiver.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_method_reference_after_an_explicit_dispose_is_memory_clean) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public X: int; "
        "  public constructor() X := 42; end constructor; "
        "  public method Hi(): int return X; end Hi; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var f := d.Hi; "
        "  d.Dispose(); "
        "  Console.WriteLine(f()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // stdoutText is the RAW captured stdout (the trailing newline is
    // Console.WriteLine's terminator) — the local idiom in this area.
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\n42\n");
}

// F2: the §21.1 `const` spelling of the method-group reference. The
// destructor must run AT THE GROUP CALL — the pre-fix build ran it at scope
// end instead (the group call was a no-op), so the output ORDER differs.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_21_1_const_class_dispose_group_call_runs_the_destructor) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class D "
            "  public constructor() end constructor; "
            "  destructor Console.WriteLine(\"disposed\"); end destructor; "
            "end D; "
            "class C public static method Main(): int "
            "  var d := new D(); "
            "  const f := d.Dispose; "
            "  f(); "
            "  Console.WriteLine(\"after-call\"); "
            "  return 0; "
            "end Main; end C;"),
        "disposed\nafter-call");
}

// F3: an annotation that does not match the member's own signature is
// REJECTED with a located diagnostic — the verdict sema gives an ordinary
// §27.6 method reference ("the initializer for `f` has a type that is not
// implicitly convertible to the target type (§5.1)", measured on the ordinary
// shape). The pre-fix build OVERWROTE the annotation, compiled, and answered
// wrongly (the statement emitted nothing).
BEZEL_TEST(class_dispose_method_group_7025,
           spec_27_6_mismatched_group_annotation_is_rejected) {
    auto r = runAot(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var f: (int) -> int := d.Dispose; "
        "  Console.WriteLine(f(1)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawError = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not compatible with the declared type") !=
                std::string::npos)
            sawError = true;
    }
    BEZEL_EXPECT_TRUE(sawError);
}

// ---------------------------------------------------------------------------
// fix #7025 (follow-up, §27.6 / §28.2, v0.343): the two defects the review of
// the fix-forward `62eb8ac0` found, plus the two rows the reviewer asked for.
//
// D1 — A PER-ITERATION LEAK IN A LEGAL LOOP. The fix-forward registered the
// deferred reclaim on `disposeStack_.front()` — the FUNCTION scope — and into
// ONE static alloca, so in a loop the slot was overwritten by the next
// iteration and every iteration but the last leaked its block (measured: ASan
// "144 byte(s) leaked in 9 allocation(s)" on a legal ten-iteration loop that
// was leak-free at the base). The reclaim now rides the scope that owns the
// receiver's BINDING (§28.2's holder's release point), and the binding is
// declared in the loop BODY — so that scope's end code runs INSIDE the loop
// and each iteration's block is reclaimed by that iteration's scope end.
//
// D2 — THE OWNED-PARAMETER RECEIVER. A parameter's symbol is a SIBLING of
// `sema::LocalSymbol`, not a subclass, so the borrow stamp (which lived on
// `LocalSymbol`) was never set for an `owned` parameter receiver and the
// explicit `Dispose()` reclaimed a block the group still pointed at
// (measured: ASan "heap-use-after-free … READ of size 8" in
// `disposethunk$Dispose`).
//
// THE ROWS BELOW ARE THE LOOP SHAPE, THE PARAMETER SHAPE, AND THE GROUP
// FORMED AFTER THE DISPOSAL — the three shapes the review named.
// ---------------------------------------------------------------------------

// D1: the loop shape. Ten iterations, each forming a group over the receiver
// and then disposing it explicitly. Every iteration's block must be reclaimed
// by that iteration's scope end — no leak (the pre-fix build leaked 9 of the
// 10 blocks: ASan "144 byte(s) leaked in 9 allocation(s)", exit code 99 under
// LeakSanitizer).
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_loop_group_dispose_reclaims_every_iteration) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3,4,5,6,7,8,9,10]; "
        "  for var i in a do "
        "    var d := new D(); "
        "    var f := d.Dispose; "
        "    d.Dispose(); "
        "  end; "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // Under LeakSanitizer a leak replaces the program's exit status with 99,
    // so this row is red on a leaking build.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expect;
    for (int i = 0; i < 10; ++i) expect += "disposed\n";
    expect += "after\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expect);
}

// D1 (the `using var` twin): the same loop with a `using var` receiver. The
// `using var`'s OWN scope-end disposal and the deferred reclaim share the
// receiver's slot, and the scope-end null-guard lets exactly one of them
// reclaim the block — so the row is leak-free AND double-free-free.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_loop_using_var_group_dispose_reclaims_every_iteration) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1,2,3,4,5,6,7,8,9,10]; "
        "  for var i in a do "
        "    using var d := new D(); "
        "    var f := d.Dispose; "
        "    d.Dispose(); "
        "  end; "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expect;
    for (int i = 0; i < 10; ++i) expect += "disposed\n";
    expect += "after\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expect);
}

// D2: the OWNED-PARAMETER receiver shape. §27.6: the group captures the
// instance by reference, so the parameter's block must stay live across the
// explicit `Dispose()` — §28.2: the block "stays live until the holder's
// release point" (for an `owned` parameter, the end of the method). The
// pre-fix build reclaimed the block AT the call and the group's invocation
// read freed memory (ASan "heap-use-after-free … READ of size 8").
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_owned_parameter_group_receiver_is_not_reclaimed_eagerly) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C "
        "  public static method Take(d: owned D): int "
        "    var f := d.Dispose; "
        "    d.Dispose(); "
        "    f(); "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new D())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n0\n");
}

// THE GROUP FORMED AFTER THE DISPOSAL. §28.2's use-after-dispose regime
// covers this program — the receiver is disposed before the group captures it
// — so the COMPILER warns (the row is in tests/ownership/, where the
// diagnostic is pinned). The RUNTIME obligation is that the capture must not
// be a null pointer: the pre-fix build nulled the receiver's slot at the
// dispose, the group captured null, and the invocation was a SIGSEGV
// (measured on the fix-forward). The group's invocation is the already-set
// dispose flag's no-op (§28.2: "a repeated call is the no-op").
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_group_formed_after_the_dispose_does_not_crash) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  d.Dispose(); "
        "  var f := d.Dispose; "
        "  f(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n");
}

// ---------------------------------------------------------------------------
// fix #7025 (follow-up, §28.2, v0.343): the NESTED-SCOPE shape.
//
// THE DEFECT THIS CLOSES. The rejected fix-forward registered the deferred
// reclaim on `disposeStack_.back()` — the scope of the CALL SITE — and its
// commit body claimed that scope "is the receiver's holder scope whenever the
// disposal happens in the receiver's own scope (the ordinary shape)". That is
// FALSE for every shape where the explicit `Dispose()` sits in a NESTED scope
// of the receiver's binding — a `do … end` block, an `if` branch, ordinary
// language surface. The nested scope ends FIRST, so the receiver's block was
// FREED at the nested scope's end while the holder was still live and the
// method group still pointed at it.
//
// MEASURED on the rejected tip (`012a83c1`), on these two shapes:
//   AddressSanitizer: heap-use-after-free … READ of size 8 … disposethunk$Dispose
//   freed by thread T0 here: #1 main
//
// THE RULE. §28.2 (v0.343): "An explicit `Dispose()` or direct
// `DisposeAsync()` performs **logical disposal only** — it sets the flag and
// runs the class's cleanup — and does not reclaim its receiver: the receiver's
// block stays live until the holder's release point." The HOLDER is the
// binding that owns the block, so the release point is the end of the
// BINDING's scope — never the end of a nested scope the call happens to sit
// in.
//
// The rows below are pinned with `runAotChecked`, so the AOT leg runs the
// program under AddressSanitizer/LeakSanitizer: a use-after-free fails the
// row (the harness reports "sanitizer fired" and the program's exit status is
// replaced), and a leak fails it too.
// ---------------------------------------------------------------------------

// The `do … end` block shape. The explicit `Dispose()` sits in the block; the
// group's invocation is AFTER the block. The receiver's block must still be
// live at that invocation — §28.2's "the receiver's block stays live until
// the holder's release point".
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_dispose_in_a_nested_block_keeps_the_group_receiver_live) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var f := d.Dispose; "
        "  do "
        "    d.Dispose(); "
        "  end; "
        "  f(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The explicit `Dispose()` runs the destructor once ("disposed"); the
    // group's later invocation is the already-set dispose flag's no-op
    // (§28.2: "a repeated call is the no-op"), so it prints nothing.
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n");
}

// The `if` branch shape — the same nested scope, reached through a branch.
BEZEL_TEST(class_dispose_method_group_7025,
           spec_28_2_dispose_in_an_if_branch_keeps_the_group_receiver_live) {
    auto r = runAotChecked(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var f := d.Dispose; "
        "  var x := 1; "
        "  if x > 0 then d.Dispose(); end; "
        "  f(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n");
}
