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
