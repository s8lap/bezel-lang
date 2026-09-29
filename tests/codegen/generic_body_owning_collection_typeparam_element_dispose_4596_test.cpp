// Codegen — issue #4596: an `owned` argument transferred into an OWNING
// built-in collection whose element type is the enclosing generic's TYPE
// PARAMETER is never disposed — the collection's scope-end free is emitted
// with a NULL element disposer (§28.11 / §14.1 / §14.4 / §28.3, v0.306).
//
// AUTHORITY. §28.11 v0.306, the element-slot policy table: an OWNING-family
// collection's slots are "owned" for a class/interface element category, and
// the `Dispose()` row for an owned slot reads "**disposes** every live
// element — reverse storage order for ordered collections … then releases
// storage". The element *category* is a derived property of the element TYPE
// (§3.6), and §14.4 fixes it at the INSTANTIATION ("the owning family admits
// only class/interface element types; see the instantiation-site restriction
// in §14.4"), not at the generic declaration. §28.3 makes the callee of an
// `owned` parameter responsible for discharging it, so once `Make(owned x: T)`
// transfers `x` into an owning collection, that collection's disposal is the
// whole discharge — if it does not cascade, the object leaks.
//
// THE BUG. Inside a MONOMORPHIZED generic body the collection's
// `genericArgs[0]` is still the placeholder parameter `T` — a `Named` TypeRef
// with a null `declaration`. `Emitter::elementPolicyFor` classifies by a
// declaration test (the class/interface arm) or by a builtin HEAD NAME
// (Shared/Weak/IGrouping/Subscription/Task/TCS/the collection families), so a
// bare `T` matched none of them and fell through to the Value row — null
// disposer. Every free site then emitted `bz_llist_free(l, NULL)` /
// `bz_array_free(a, NULL)` / `bz_dict_free(t, NULL)`, which reclaims the
// nodes/backing but never the elements.
//
// Read off `BEZEL_DUMP_IR=1` on the issue's repro: the monomorphized
// `Outer$Go$spec$Res` carries `call void @bz_llist_free(ptr %18, ptr null)`,
// while the same program with a CONCRETE type argument in a non-generic
// caller carries `call void @bz_llist_free(ptr %21, ptr @"bz$elemDispose")`.
//
// SCOPE, narrowed by measurement (each loop-amplified to 500 iterations, LSan
// on the generated code via `bezelc --sanitize=address`):
//   * `Inner<T>.Make(owned x: T)` building a `using var` OwningLinkedList<T>
//     — 8,000 B / 500 allocations leaked. The issue's two-frame static chain
//     is NOT required; ONE generic frame suffices.
//   * the same with OwningArray<T> — 8,000 B / 500 allocations.
//   * the issue's verbatim `Outer<Res>.Go(new Res())` chain — 7,984 B / 499.
//   * the instance-receiver spelling of the same chain — same leak, so the
//     static-member-access receiver of #4582 is NOT the discriminator.
//   * the non-generic `OwningLinkedList<Res>` twin, and `Inner<Res>.Make`
//     called from a non-generic `Main`, are both CLEAN on the pre-fix
//     compiler — the defect is exactly "element type is an unresolved type
//     parameter at the free site".
// ⚠ The issue's own ONE-SHOT repro is a vacuous oracle in the other
// direction too: the single-generic-frame shape reports LSan-clean at one
// iteration on BOTH compilers (the pointer is still live in `main`'s stack
// frame, so LSan calls it reachable) and only shows the leak once looped.
// Every row below therefore loops and asserts an exact destructor count
// rather than relying on the sanitizer.
//
// THE FIX resolves the element ref through the active monomorphization —
// `elemTy = resolveTypeParamRef(elemTy)` — at the TOP of `elementPolicyFor`,
// the file that owns the single source of truth for element-slot behaviour.
// The per-site version of this fix already exists and was already known to be
// insufficient: `stampOwningElemDisposer` carries #816's local
// `elementPolicyFor(resolveTypeParamRef(elemTy), true)`, which repairs only
// the construction-time `bz_array_set_elem_disposer` header stamp and leaves
// every static free site reading the unresolved parameter.
//
// FAIL-WITHOUT proved mechanically, at the gtest level: with
// `LlvmBackend_ElementPolicy.cpp` restored to this worktree's pre-fix HEAD
// (673f9b72) and swapped back into the compiler archive, this file scores
// **8 failed / 4 passed / 12 total**. The eight failures are every
// `*_disposes` row — each returns 0 where it expects 200 (or 50), i.e. NOTHING
// is disposed — plus `loop_amplified_500x_chain_leak_free`, whose LSan report
// reads "7984 byte(s) leaked in 499 allocation(s)", the issue's own
// measurement scaled to 500 iterations. The four passes are exactly the
// `*_control` rows: they are the shapes that already worked, and they are what
// proves the fix did not widen an owned slot into a borrowing one (or the
// reverse). With the fix, 12 passed / 0 failed, and the run reports
// "CHECKED TOOLCHAIN: 0 skip(s)" and "NATIVE SANITIZER: 0 check(s) skipped",
// so the sanitizer leg really ran rather than being silently absent.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "  diag: " << d.message << "\n";
        return -999;
    }
    return r.exitCode;
}

// The LSan leg — `runAotChecked` flips `ran` to false on a leak, so the -999
// sentinel fails the row on the sanitizer axis independently of the counter.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "  diag: " << d.message << "\n";
        return -999;
    }
    return r.exitCode;
}

// A class whose destructor counts disposals. A missed disposal and a doubled
// one are both an EXACT wrong number, so the oracle does not depend on the
// sanitizer being live. 200 iterations keeps the count inside the 8-bit
// `WEXITSTATUS` mask (§ the harness note in tests/README.md).
constexpr const char* kRes =
    "module M; "
    "class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; ";

// 1. ISSUE REPRO — the two-frame static generic chain from #4596, verbatim in
//    shape (`Outer<Res>.Go(new Res())` → `Inner<T>.Make(owned x)` →
//    `OwningLinkedList<T>`), with the destructor counter added and the call
//    looped. The `owned` argument crosses BOTH static generic frames before
//    reaching the owning collection, and `Go`'s `using var l` is the only
//    disposal point in the program. Pre-fix: 0. Post-fix: 200.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           static_generic_chain_owned_arg_disposes) {
    std::string src = std::string(kRes) +
        "public class Inner<T> where T: class "
        "  public static method Make(owned x: T): owned OwningLinkedList<T> "
        "    var l := new OwningLinkedList<T>(); "
        "    l.AddLast(owned x); "
        "    return l; "
        "  end Make; "
        "end Inner; "
        "public class Outer<T> where T: class "
        "  public static method Go(owned x: T): int "
        "    using var l := Inner<T>.Make(owned x); "
        "    return l.Count + 2; "
        "  end Go; "
        "end Outer; "
        "class C public static method Main(): int "
        "  for var i in 0..<200 do "
        "    _ := Outer<Res>.Go(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 2. The issue's stated VALUE, on the issue's exact program: `l.Count + 2`
//    over a one-element list is 3. The pre-fix compiler already returned 3 —
//    this row pins that the fix did not change the observable answer while
//    adding the disposal (a fix that, say, disposed the element before
//    `l.Count` was read would break it).
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           static_generic_chain_value_unchanged_control) {
    constexpr const char* kSrc =
        "module M; "
        "class Res public n: int := 0; end Res; "
        "public class Inner<T> where T: class "
        "  public static method Make(owned x: T): owned OwningLinkedList<T> "
        "    var l := new OwningLinkedList<T>(); "
        "    l.AddLast(owned x); "
        "    return l; "
        "  end Make; "
        "end Inner; "
        "public class Outer<T> where T: class "
        "  public static method Go(owned x: T): int "
        "    using var l := Inner<T>.Make(owned x); "
        "    return l.Count + 2; "
        "  end Go; "
        "end Outer; "
        "class C public static method Main(): int "
        "  return Outer<Res>.Go(new Res()); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(kSrc), 3);
}

// 3. NARROWED REPRO — ONE generic frame is enough. `Inner<T>.Make` takes the
//    `owned` argument and disposes it through its own `using var`
//    OwningLinkedList<T>; there is no chain at all. Pre-fix: 0. Post-fix: 200.
//    This is the row that proves the defect is the unresolved element type at
//    the free site, not the static-member-access receiver shape of #4582.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           single_generic_frame_owninglinkedlist_disposes) {
    std::string src = std::string(kRes) +
        "public class Inner<T> where T: class "
        "  public static method Make(owned x: T): int "
        "    using var l := new OwningLinkedList<T>(); "
        "    l.AddLast(owned x); "
        "    return l.Count; "
        "  end Make; "
        "end Inner; "
        "class C public static method Main(): int "
        "  for var i in 0..<200 do "
        "    _ := Inner<Res>.Make(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 4. The bz_array_t-backed sibling: OwningArray<T> in the same generic body
//    goes through a DIFFERENT free site (`bz_array_free` rather than
//    `bz_llist_free`), reading the same policy. Pre-fix: 0. Post-fix: 200.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           single_generic_frame_owningarray_disposes) {
    std::string src = std::string(kRes) +
        "public class Inner<T> where T: class "
        "  public static method Make(owned x: T): int "
        "    using var a := new OwningArray<T>(); "
        "    a.Add(owned x); "
        "    return a.Count; "
        "  end Make; "
        "end Inner; "
        "class C public static method Main(): int "
        "  for var i in 0..<200 do "
        "    _ := Inner<Res>.Make(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 5. The dict-backed sibling: an OwningDictionary<int, T> VALUE slot (§28.11
//    "For dictionaries the classification applies independently to the key
//    slots and the value slots: an owning dictionary owns its *values*").
//    `bz_dict_free`'s value disposer reads the same policy on `genericArgs[1]`.
//    Pre-fix: 0. Post-fix: 200.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           single_generic_frame_owningdictionary_value_disposes) {
    std::string src = std::string(kRes) +
        "public class Box<T> where T: class "
        "  public static method Stash(owned x: T): int "
        "    using var d := new OwningDictionary<int, T>(); "
        "    d.Add(1, owned x); "
        "    return d.Count; "
        "  end Stash; "
        "end Box; "
        "class C public static method Main(): int "
        "  for var i in 0..<200 do "
        "    _ := Box<Res>.Stash(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 6. The INSTANCE-receiver spelling of row 1's chain. #4582 is about the
//    static-member-access receiver (`Outer<Res>.Go`); this row takes that
//    receiver out of the picture entirely (`o.Go(...)` on an instance) and
//    still leaks pre-fix, which is how the two issues were told apart.
//    Pre-fix: 0. Post-fix: 200.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           instance_receiver_generic_chain_disposes) {
    std::string src = std::string(kRes) +
        "public class Inner<T> where T: class "
        "  public static method Make(owned x: T): owned OwningLinkedList<T> "
        "    var l := new OwningLinkedList<T>(); "
        "    l.AddLast(owned x); "
        "    return l; "
        "  end Make; "
        "end Inner; "
        "public class Outer<T> where T: class "
        "  public method Go(owned x: T): int "
        "    using var l := Inner<T>.Make(owned x); "
        "    return l.Count + 2; "
        "  end Go; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var o := new Outer<Res>(); "
        "  for var i in 0..<200 do "
        "    _ := o.Go(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 7. The OTHER half of §28.11's "class / interface type" row — the same
//    generic body at `T = IThing`, an INTERFACE substitution. The policy's
//    class arm covers Class|Interface alike (fix #619), so resolving the
//    parameter must reach it for an interface argument too; the disposal is
//    the same vtable dispatch. Pre-fix: 0. Post-fix: 200.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           interface_substitution_disposes) {
    std::string src =
        "module M; "
        "public interface IThing "
        "  method Val(): int; "
        "end IThing; "
        "class Res: IThing "
        "  public static _n: int := 0; "
        "  public method Val(): int => 5; "
        "  destructor _n := _n + 1; end destructor; "
        "end Res; "
        "public class Inner<T> where T: class "
        "  public static method Make(owned x: T): int "
        "    using var l := new OwningLinkedList<T>(); "
        "    l.AddLast(owned x); "
        "    return l.Count; "
        "  end Make; "
        "end Inner; "
        "class C public static method Main(): int "
        "  for var i in 0..<200 do "
        "    _ := Inner<IThing>.Make(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 8. A generic METHOD's own type parameter, on a NON-generic class — the
//    other way a substitution becomes active (§14.1 admits type parameters on
//    both declarations). The element type is again a bare `T` at the free
//    site. Pre-fix: 0. Post-fix: 50.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           generic_method_typeparam_disposes) {
    std::string src = std::string(kRes) +
        "public class U "
        "  public static method Make<T>(owned x: T): int where T: class "
        "    using var l := new OwningLinkedList<T>(); "
        "    l.AddLast(owned x); "
        "    return l.Count; "
        "  end Make; "
        "end U; "
        "class C public static method Main(): int "
        "  for var i in 0..<50 do "
        "    _ := U.Make<Res>(new Res()); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 50);
}

// 9. NO-OVER-DISPOSE GUARD — the BORROWING family in the same generic body.
//    `Array<T>` borrows its elements (§28.11: the borrow-slot `Dispose()`
//    column is "no user-visible work; elements untouched"), and the element's
//    real owner is the caller's `using var r`. Resolving `T` to `Res` must
//    NOT turn that borrow into an owned slot — if it did, each `Res` would be
//    disposed twice and the counter would read 400 (144 after the 8-bit mask)
//    with an ASan double-free besides. 200 on BOTH compilers.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           borrowing_array_typeparam_does_not_dispose_control) {
    std::string src = std::string(kRes) +
        "public class Holder<T> where T: class "
        "  public static method Keep(x: T): int "
        "    using var a := new Array<T>(); "
        "    a.Add(x); "
        "    return a.Count; "
        "  end Keep; "
        "end Holder; "
        "class C public static method Main(): int "
        "  for var i in 0..<200 do "
        "    using var r := new Res(); "
        "    _ := Holder<Res>.Keep(r); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 10. CONTROL at the shape that already worked — the same body with a CONCRETE
//    element type in a NON-generic class. `genericArgs[0]` is already `Res`
//    there, `resolveTypeParamRef` returns it unchanged, and the free site
//    always carried the element disposer. 200 on BOTH compilers.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           nongeneric_concrete_element_control) {
    std::string src = std::string(kRes) +
        "class C "
        "  public static method Make(owned x: Res): int "
        "    using var l := new OwningLinkedList<Res>(); "
        "    l.AddLast(owned x); "
        "    return l.Count; "
        "  end Make; "
        "  public static method Main(): int "
        "    for var i in 0..<200 do "
        "      _ := Make(new Res()); "
        "    end; "
        "    return Res._n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 200);
}

// 11. RUNTIME-MANAGED ROW GUARD — a BORROWING `LinkedList<T>` at `T = string`
//    inside a generic body. §28.11 puts a `string` in a borrowing collection
//    in the runtime-managed row ("shared references"), and the retain half
//    (`llIsStrElem` at the `AddLast` site) and the release half (the
//    scope-end `stringElementReleaseThunk`) BOTH read `elementPolicyFor` on
//    the same `genericArgs[0]`. Resolving centrally flips both together, so
//    the co-ownership stays balanced; resolving at only one of them would
//    over-release (a use-after-free on the string payload). 400 stored
//    elements, `400 mod 251` = 149 on BOTH compilers, and clean under LSan.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           borrowing_linkedlist_string_typeparam_balanced_control) {
    constexpr const char* kSrc =
        "module M; "
        "public class Bag<T> "
        "  public static method Count2(a: T, b: T): int "
        "    using var l := new LinkedList<T>(); "
        "    l.AddLast(a); "
        "    l.AddLast(b); "
        "    return l.Count; "
        "  end Count2; "
        "end Bag; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  for var i in 0..<200 do "
        "    total := total + Bag<string>.Count2($\"a{i}\", \"b\"); "
        "  end; "
        "  return total mod 251; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(kSrc), 149);
}

// 12. LOOP-AMPLIFIED LEAK ORACLE — the issue's own chain at 500 iterations
//     under `runAotChecked` (ASan/LSan on the generated code). This is the
//     row that reproduces the issue's measurement directly: 7,984 B in 499
//     allocations pre-fix (one of the 500 stays reachable from `main`'s
//     stack, which is exactly why the issue's one-shot repro is not enough
//     on its own). LSan-clean post-fix; 500 × 3 = 1500, `1500 mod 251` = 245.
BEZEL_TEST(generic_body_owning_collection_typeparam_element_dispose_4596,
           loop_amplified_500x_chain_leak_free) {
    constexpr const char* kSrc =
        "module M; "
        "class Res public n: int := 0; end Res; "
        "public class Inner<T> where T: class "
        "  public static method Make(owned x: T): owned OwningLinkedList<T> "
        "    var l := new OwningLinkedList<T>(); "
        "    l.AddLast(owned x); "
        "    return l; "
        "  end Make; "
        "end Inner; "
        "public class Outer<T> where T: class "
        "  public static method Go(owned x: T): int "
        "    using var l := Inner<T>.Make(owned x); "
        "    return l.Count + 2; "
        "  end Go; "
        "end Outer; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 0..<500 do "
        "    acc := acc + Outer<Res>.Go(new Res()); "
        "  end; "
        "  return acc mod 251; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(kSrc), 245);
}

}  // namespace
