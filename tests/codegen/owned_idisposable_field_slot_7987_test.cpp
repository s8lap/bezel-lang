// Spec-derived regression for issue #7987: an `owned IDisposable?` FIELD and
// an owning collection's `IDisposable` SLOT are never disposed — the value's
// destructor never runs.
//
// THE RULE
// ========
// §28.1 (v0.357) — the `owned` type requirement: "In the method-parameter,
// return-type, `out`-parameter, lambda-parameter, choice-payload, and
// function-type parameter/return slot positions above, `owned` requires the
// declared type to be a **class type other than `Type`**, **an interface
// type**, or a bare type parameter."  `IDisposable` is an interface type.
//
// §28.4 step 2 (v0.357) — the generated `Dispose()`: "Disposes all
// owned-bearing fields in reverse declaration order. This covers fields marked
// `owned` (class- or interface-typed; the field's value's `Dispose()` is
// called)".
//
// §28.5 (v0.357) — owned fields: the `owned` modifier "**Auto-disposes** the
// field when the containing object is disposed (after the destructor body
// runs, in reverse declaration order; a nullable field is null-checked before
// disposal, and a null field disposes nothing)"; and "Only fields whose type
// is a class type or an interface type take the `owned` field modifier as a
// marking of their own".
//
// §28.7 (v0.357) — reassigning owned fields: the value the store displaces
// is disposed by the store.
//
// §28.11 (v0.357) — the element slot policy: "class / interface type |
// borrows | **owned**", and the collection's `Dispose()` "**disposes** every
// live element".  `OwningArray<T>` is the owning family; `Array<T>` is the
// borrowing family, whose elements "are never disposed — nor disposable
// through the collection" (§28.11 / §28.9's table), and §15.4's fresh-store
// rule rejects storing an unbound owned temporary into one.
//
// THE DEFECT (measured on base 4a6919f16 — the r30-c1 tip, spec v0.357)
// =====================================================================
// `IDisposable` / `IAsyncDisposable` are DECLARATION-LESS built-in interface
// heads (`kBuiltinInterfaceHeads`, src/common/BuiltinRegistry.cpp:557 —
// deliberately kept OUT of the vtable-less set by #4326).  Every classifier
// that keyed on `TypeRef::declaration != nullptr` therefore skipped them:
//
//   * `Emitter::isOwnedReferenceField` (src/codegen/LlvmBackend_impl.hpp) —
//     so `emitOwnedFieldDisposals` SKIPPED an `owned F: IDisposable?` field:
//     the value leaked and its destructor never ran (25 600 B / 400
//     allocations for 200 CancellationTokenSources; 3 200 B / 200 for a plain
//     user class);
//   * `Emitter::elementPolicyFor` (src/codegen/LlvmBackend_ElementPolicy.cpp)
//     — so an `OwningArray<IDisposable>` element fell to the Value row (a null
//     disposer) and `bz_array_free` was called with a NULL element disposer:
//     every element leaked.
//
// The `Object`-typed analogue of the field half was fixed by #8118 with a
// separate `isOwnedObjectField` predicate + a runtime-discrimination
// discharge; the `IDisposable` head had no such arm.
//
// THE FIX
// =======
// `isOwnedReferenceField` now admits the declaration-less disposal heads
// (`isDisposableInterfaceHeadType`), and `elementPolicyFor`'s class/interface
// row admits them as an element type.  The discharge is the vtable slot-2
// dynamic dispatch — the SAME path the `owned d: IDisposable` PARAMETER case
// takes (#4328 / #7554) and the same path the `using var`-bound LOCAL case
// takes.
//
// That dispatch presumes the slot's value opens with a vtable.  It is NOT
// true that such a value is always a user class: the stdlib's classes
// implement `IDisposable` too (stdlib §2.10) and several had no dispatch
// header when this fix was written — `Shared<T>` / `Weak<T>`, `LinkedList`,
// the hash-backed `Dictionary` / `Set` families, the event sources, the
// atomic cells, the serializers, `Subscription`, `Channel<T>`, the
// completion sources — so for those this fix turned a leak into a wild
// dispatch (#8533).  Fix #8533 gave each of them except `Channel<T>` and the
// completion sources a word-0 vtable; its rows are in
// tests/codegen/idisposable_slot_headerless_handle_8533_test.cpp.  Fix #8608
// gave `Channel<T>`, `ChannelSender<T>` and the completion sources theirs;
// its rows are in tests/codegen/idisposable_slot_channel_tcs_8608_test.cpp.
//
// THE BORROWING FAMILY IS NOT TOUCHED
// ===================================
// `Array<IDisposable>` keeps the borrow row (a null disposer): a borrowing
// collection's slots are borrows and are never disposed, and §15.4's
// fresh-store rule rejects an unbound owned temporary at the store site.  The
// last row below pins that rejection, so a future widening of the owning row
// cannot silently make the borrowing family own.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The declarations every row shares; `main` is the body of `Main`.
std::string prog(const std::string& main) {
    return
        "module M;\n"
        "import System.Threading;\n"
        "public class Res\n"
        "  public constructor() end constructor;\n"
        "  destructor\n"
        "    Console.WriteLine(\"RD\");\n"
        "  end destructor;\n"
        "end Res;\n"
        "public class H\n"
        "  public owned F: IDisposable? := null;\n"
        "  public constructor() end constructor;\n"
        "end H;\n"
        "public class HN\n"
        "  public owned F: IDisposable;\n"
        "  public constructor() F := new CancellationTokenSource(); end constructor;\n"
        "end HN;\n"
        "public class C\n"
        "  public static method Main(): int\n" +
        main +
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

bool hasError(const JitResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The row must RUN, must be ASan/LSan-clean (no leak, no double free), and
// must print exactly `out`.
//
// NOTE ON THE SKIP: there is deliberately NO absorbing `if (skipped) return;`
// guard here (the `check_toolchain_skip_surface` ratchet counts such a site
// as a violation, and the lane contract forbids it): a toolchain SKIP must
// surface as a FAILURE, not as a silent pass.  `checkedToolchainSkipped` is
// asserted FALSE, and `ran` is asserted TRUE, so a SKIP fails the row.
void expectCleanUnderAsan(const std::string& program, const std::string& out) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ---- §28.4 step 2 / §28.5: the `owned F: IDisposable?` field --------------

// The field's value's `Dispose()` is called when the containing object is
// disposed — so the value's destructor RUNS.  On base it never ran and the
// value leaked (3 200 B / 200 allocations, no "RD").
BEZEL_TEST(owned_idisposable_field_slot_7987, spec_28_4_2_owned_field_disposes_its_value) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 50 do\n"
             "      var h := new H();\n"
             "      h.F := new Res();\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n"),
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "end\n");
}

// The issue's own measurement shape: a `CancellationTokenSource` in an
// `owned F: IDisposable?` field.  On base this leaked 25 600 B / 400
// allocations for 200 sources (the direct cell + the indirect allocation).
BEZEL_TEST(owned_idisposable_field_slot_7987, spec_28_4_2_owned_field_cts_is_reclaimed) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 50 do\n"
             "      var h := new H();\n"
             "      h.F := new CancellationTokenSource();\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n"),
        "end\n");
}

// The NON-NULLABLE owned field (§28.5's "a nullable field is null-checked
// before disposal" — the non-nullable form has no null to check).  On base
// this leaked 25 600 B / 400 allocations.
BEZEL_TEST(owned_idisposable_field_slot_7987, spec_28_5_owned_field_non_nullable_is_disposed) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 50 do\n"
             "      var h := new HN();\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n"),
        "end\n");
}

// §28.7: the value a store DISPLACES is disposed by the store.  On base both
// the displaced value and the field's value leaked (96 B / 6 allocations for
// three iterations); here BOTH destructors run — the displaced one at the
// store, the field's one at the object's disposal.
BEZEL_TEST(owned_idisposable_field_slot_7987, spec_28_7_owned_field_overwrite_disposes_the_displaced_value) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 50 do\n"
             "      var h := new H();\n"
             "      h.F := new Res();\n"
             "      h.F := new Res();\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n"),
        // TWO per iteration: the value the second store displaces (§28.7) and
        // the field's value at the object's disposal (§28.4 step 2).
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "end\n");
}

// ---- §28.11: the owning collection's `IDisposable` slot --------------------

// §28.11's element slot policy: a class/interface element type in the OWNING
// family is an owned slot, and the collection's `Dispose()` "disposes every
// live element".  On base the element fell to the Value row (a null
// disposer) and the value leaked (48 B / 3 allocations for three iterations).
BEZEL_TEST(owned_idisposable_field_slot_7987, spec_28_11_owning_array_disposes_every_element) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 50 do\n"
             "      var a := new OwningArray<IDisposable>();\n"
             "      a.Add(new Res());\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n"),
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "end\n");
}

// The issue's own collection measurement shape: a `CancellationTokenSource`
// in an `OwningArray<IDisposable>`.  On base this leaked 25 600 B / 400
// allocations for 200 sources.
BEZEL_TEST(owned_idisposable_field_slot_7987, spec_28_11_owning_array_cts_is_reclaimed) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 50 do\n"
             "      var a := new OwningArray<IDisposable>();\n"
             "      a.Add(new CancellationTokenSource());\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n"),
        "end\n");
}

// §28.11's slot table: the owning family's `Remove` "disposes the removed
// element".  On base the removed element leaked (3 200 B / 200 allocations).
BEZEL_TEST(owned_idisposable_field_slot_7987, spec_28_11_owning_array_remove_disposes_the_removed_element) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 50 do\n"
             "      var a := new OwningArray<IDisposable>();\n"
             "      a.Add(new Res());\n"
             "      var removed := a.RemoveAt(0);\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n"),
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "RD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\nRD\n"
        "end\n");
}

// ---- the BORROWING family, unchanged (§28.11 / §15.4) ----------------------
//
// The issue's literal `Array<IDisposable>` shape.  A borrowing collection's
// slot is a BORROW row ("Elements are borrows and are never disposed — nor
// disposable through the collection", §28.9's table), and §15.4's
// fresh-store rule rejects an unbound owned temporary at the store site — so
// this program does not compile, and the element is never owned by the
// collection.  This row pins that the borrowing family is NOT widened by the
// fix: an `Array<IDisposable>` must not start owning its elements.
BEZEL_TEST(owned_idisposable_field_slot_7987, spec_15_4_borrowing_array_of_idisposable_rejects_a_fresh_store) {
    JitResult r = runAotChecked(
        prog("    var a := new Array<IDisposable>();\n"
             "    a.Add(new CancellationTokenSource());\n"
             "    Console.WriteLine(\"end\");\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "fresh unbound owned temporary"));
    BEZEL_EXPECT_TRUE(hasError(r, "borrowing-family collection"));
}
