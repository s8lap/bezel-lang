// Codegen — §8.1 / §28.4 / §28.5 / §28.9 / §28.11 / §12.4, v0.338 (#6402): an
// `owned` FIELD whose declared type is a built-in LIST INTERFACE HEAD
// (`IList<T>` / `IOwningList<T>` / `IReadOnlyList<T>`) SEGVs when the slot's
// runtime value is a USER CLASS implementing that head.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Disposal.cpp, `emitOwnedFieldDisposals`).
// The `owned` collection-field arm assumed every `it->collTy` denotes a
// header-less C struct (bz_array_t / bz_llist_t / bz_dict_t) and emitted the
// matching free. That is true of every CONCRETE built-in name, because those
// are sealed — `class MyL: Array<int>` is rejected outright:
//
//   cannot inherit from compiler-known class `Array`: it is closed to
//   inheritance, as a `sealed` class is … (§12.4, §31.4)
//
// (MEASURED for both `Array<int>` and `OwningArray<Cell>`.) It is NOT true of
// the list interface heads. `kBuiltinInterfaceHeads` (#2921 / #2923,
// src/common/BuiltinRegistry.cpp) records `IList` / `IOwningList` /
// `IReadOnlyList` as EXACTLY the reserved names whose runtime value may be a
// user class — the precise COMPLEMENT of `isVtableLessBuiltinClassName` — and
// §8.1 makes `class MyL: IList<int>` legal, so sema accepts the assignment with
// NO diagnostic. On such a value the object's word 0 is a VTABLE POINTER, so
// `arr->data` / `arr->count` / `arr->elem_disposer` are all read OFF THE
// VTABLE: this is a WILD FREE, not a leak.
//
// This is the FIELD-site twin of #6368 (the async `owned`-capture site), whose
// fix-forward declined the list heads there because the static type does not
// determine the runtime kind. DECLINING IS NOT AN OPTION AT THE FIELD SITE, and
// that is the finding this file's controls pin: the array-backed list-interface
// field is a spec-legitimate, currently LSan-clean, PINNED spelling
// (`codegen_owned_ilist_field_backing_leak_2603`,
// `codegen_owned_iowninglist_field_backing_leak_2606`,
// `codegen_owned_polymorphic_list_iface_field_element_leak_2608`,
// `codegen_owning_literal_elem_disposer_2635`), so removing the array free
// would have traded a crash for a leak on a program that works today. The fix
// therefore DISCRIMINATES AT RUN TIME instead of narrowing the static type.
//
// THE DISCRIMINATOR is `bz_obj_is_string(val, vtableTablePtr())` — the same
// helper and the same emitted vtable table #417 uses to separate an `Object`
// slot's string from its class instance, and the same one the
// IEquatable<string> / IComparable<string> itable site uses. Its contract is a
// whitelist NEGATION, so it is sound only where the slot admits a closed
// population — which is exactly this one, and by the same argument #417's own
// site records: in an `IList<int>` slot the only admitted values are a concrete
// built-in collection (word 0 = `arr->data`, a heap pointer, never a listed
// vtable; NULL for an empty backing, which the null-terminator scan stops at) and
// a user class declaring the head (whose vtable IS registered — user and
// imported alike, see #1843's `finalizeVtableTable` for the lazily-created
// specialized vtables). Unlike the `Object` case there is no root-type escape,
// so the "receiver reachability is not determined by what the object declares"
// caveat does not apply. The user-class arm is the SAME
// `emitDisposeAndReclaim` + null-after pair the class-instance arm at the
// bottom of that same loop uses.
//
// MEASURED fail-without on the base (this tree with the fix reverted, `bezelc`
// rebuilt, 500 constructions per row): every user-class row below SEGVs —
//
//   AddressSanitizer: SEGV … in __asan::Allocator::Deallocate <- free <-
//   bz_array_free_dynamic   (bezel_runtime_collections.cpp:351)
//   … or <- bz_array_free (the elemDisposer / owning-element paths)
//   Segmentation fault (core dumped), exit 139, under the PLAIN oracle too
//
// so the pre-fix outcome is a hard crash on BOTH oracles, not a warning.
//
// WHY EVERY ROW ARMS THE LEAK ORACLE (`LeakEnv("strict")`, below). The defect
// here is a wild free, but the fix MOVES a release: before it, a user class in
// this slot was (accidentally) "released" by the array free; after it, the
// release is `emitDisposeAndReclaim`, which must free the user class AND let
// its own `owned` fields die with it. A fix that merely stopped crashing by
// declining the free would leak, and `runAotChecked` DEFAULTS to
// `ASAN_OPTIONS=detect_leaks=0` — it would go green on exactly that broken
// tree. Arming the knob is what makes the "and does not leak" half real. Each
// row accumulates 500 distinct heap objects, far above the 50-500 one-shot band,
// and every row returns a value derived from those 500 so a partial discharge
// shows up as a wrong exit code too.
//
// 500 x 1 = 500; 500 mod 251 = 249.

#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row — the gate may run a whole
// shard with the knob set, and a row must not pollute or inherit it. Copied
// from todictionary_duplicate_key_leak_5421_test.cpp.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// The ONE runAotChecked call site in this file. It does NOT absorb a toolchain
// skip: `checkedToolchainSkipped` is asserted FALSE BEFORE anything could
// swallow it, so a box whose ASan link is unavailable fails the row loudly
// instead of reporting a vacuous green. There is no `if (!r.ran) return;`.
//
// Both halves are asserted: `ran` says the program ran to normal termination
// under ASan/LSan with no leak and no memory error; the exit code says it
// computed the spec-derived value. Either half alone is satisfiable vacuously.
void expectClean(const std::string& src, int expected) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

// The `owned` user-class member that makes `MyL` a genuine §8.1 implementer
// rather than a marker: it holds a real collection of its own, so a correct
// release has to reach through the destructor and free THAT too. `MyL` is
// private, so its vtable has internal linkage — the #1843 registration case
// that `finalizeVtableTable` exists for.
constexpr const char* kIListImpl =
    "class MyL: IList<int> "
    "  private owned _a: Array<int>; "
    "  public constructor() _a := new Array<int>(1); end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return _a.GetEnumerator(); "
    "  end GetEnumerator; "
    "end MyL; ";

}  // namespace

// §8.1 / §28.4 (#6402) — THE HEADLINE SHAPE, the issue's own repro. The field's
// declared type is a borrowing list head and the runtime value is `MyL`, a
// vtable-bearing user class. Pre-fix this SEGVs in `bz_array_free_dynamic`
// (no element disposer is derivable for an `int` element, so the #2608 dynamic
// branch is taken). Post-fix the class is disposed through its own vtable,
// which frees `MyL` and its `owned _a: Array<int>` — LSan-clean.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_user_class_disposal_6402,
           owned_IList_field_user_class_disposed_not_array_freed_spec_8_1) {
    std::string src =
        "module M; " + std::string(kIListImpl) +
        "class Holder "
        "  private owned _f: IList<int>; "
        "  public constructor() _f := new MyL(); end constructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §8.1 / §28.5 (#6402) — the READ-ONLY head. Same defect, third spelling: it is
// in `kBuiltinInterfaceHeads` for the same reason, and it reaches the same
// #2608 dynamic branch. Pinned separately so a fix that special-cased `IList`
// alone would leave this red.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_user_class_disposal_6402,
           owned_IReadOnlyList_field_user_class_disposed_spec_8_1) {
    std::string src =
        "module M; "
        "class MyL: IReadOnlyList<int> "
        "  private owned _a: Array<int>; "
        "  public constructor() _a := new Array<int>(1); end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end MyL; "
        "class Holder "
        "  private owned _f: IReadOnlyList<int>; "
        "  public constructor() _f := new MyL(); end constructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §8.1 / §28.5 / §28.11 (#6402) — the OWNING head, on a CLASS element. This row
// is the discriminator against the OVER-DISPOSE direction: `IOwningList<Cell>`
// makes the field's family owning, so the pre-fix code derived a §28.11 OWNING
// element disposer and called `bz_array_free(val, disp)` — reading a length and
// a disposer off the vtable AND, had the address survived, cascading a dispose
// into elements this list does not own. `MyL`'s own `_a` is a borrowing
// `Array<Cell>`, so the correct release disposes NO element: the rows' LSan
// cleanliness with a non-empty element population is what proves the cascade
// did not run.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_user_class_disposal_6402,
           owned_IOwningList_field_user_class_disposed_no_elem_cascade_8_1) {
    std::string src =
        "module M; "
        "class Cell "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method N(): int return _n; end N; "
        "end Cell; "
        "class MyL: IOwningList<Cell> "
        "  private owned _a: Array<Cell>; "
        "  public constructor() _a := new Array<Cell>(); end constructor; "
        "  public method GetEnumerator(): IEnumerator<Cell> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end MyL; "
        "class Holder "
        "  private owned _f: IOwningList<Cell>; "
        "  public constructor() _f := new MyL(); end constructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §8.1 / §28.9 (#6402) — the STRING-element branch, which is a DIFFERENT emit
// path inside the same arm. A `string` element derives a live disposer
// (`stringElementReleaseThunk`, §28.11's runtime-managed row), so the #2608
// `!elemDisp` dynamic branch is NOT taken and the code falls through to the
// plain `bz_array_free(val, disp)`. Pre-fix that SEGVs in `bz_array_free`
// instead — a second, independently-reached crash the headline row does not
// cover. The added element also gives `MyL` a real per-element +1, so LSan
// cleanliness here shows the string reference is released exactly once (on
// `MyL`'s own field arm) rather than leaked or double-released.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_user_class_disposal_6402,
           owned_IList_string_elem_field_user_class_disposed_spec_28_9) {
    std::string src =
        "module M; "
        "class MyL: IList<string> "
        "  private owned _a: Array<string>; "
        "  public constructor() "
        "    _a := new Array<string>(); "
        "    _a.Add(\"hello\"); "
        "  end constructor; "
        "  public method GetEnumerator(): IEnumerator<string> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end MyL; "
        "class Holder "
        "  private owned _f: IList<string>; "
        "  public constructor() _f := new MyL(); end constructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §8.1 / §28.5 (#6402) — the CAPTURED PRIMARY-CTOR-PARAMETER spelling, which is
// the SAME emission site reached by a different door. `ownedCollectionFieldType`
// admits the list heads on its `fs->posParam` arm as well as its FieldDecl arm,
// so `class Holder(owned v: IList<int>)` arrives at the collection arm with the
// same `it->collTy` and is fixed by the same branch. Pinned because the two
// arms of that predicate can drift.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_user_class_disposal_6402,
           owned_IList_ctor_param_user_class_disposed_spec_8_1) {
    std::string src =
        "module M; " + std::string(kIListImpl) +
        "class Holder(owned v: IList<int>) "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(new MyL()); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §28.11 (#6402) — CONTROL 1, the array-backed half of `IList`. The runtime
// value really is a `bz_array_t*`, so the correct release really IS
// `bz_array_free_dynamic`, and the discriminator must keep taking the array
// arm. This row is GREEN BEFORE THE FIX AND AFTER IT, which is the point: it is
// what rules out the "narrow the static type" fix that #6368 used at the
// capture end. Narrowing would turn this program into a leak (and turn the four
// suites named in the header red).
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_user_class_disposal_6402,
           owned_IList_field_array_backing_still_freed_spec_28_11) {
    std::string src =
        "module M; "
        "class Holder "
        "  private owned _f: IList<int>; "
        "  public constructor() _f := new Array<int>(3); end constructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §28.11 (#6402) — CONTROL 2, the CLASS-element array-backed half. This is
// `#2608`'s own row and the one that pins the #2608 dynamic branch itself: a
// borrowing collection with a class element derives NO disposer, so it takes
// `bz_array_free_dynamic` with a null recorded header disposer. The fix must
// leave that path byte-identical. GREEN BOTH SIDES, like control 1.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_user_class_disposal_6402,
           owned_IList_class_elem_array_backing_still_freed_spec_28_11) {
    std::string src =
        "module M; "
        "class Cell "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method N(): int return _n; end N; "
        "end Cell; "
        "class Holder "
        "  private owned _f: IList<Cell>; "
        "  public constructor() _f := new Array<Cell>(); end constructor; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §8.1 / §12.4 (#6402) — CONTROL 3, the CONCRETE-name half of the sealed-class
// argument. `MyL` here really IS a user class, but the slot is typed by the
// CONCRETE `Array<int>`, which §12.4/§31.4 closes to inheritance — so the
// assignment is rejected outright and no array free is ever reached. This is
// the row that establishes the fix's premise is narrow: the discriminator is
// only needed for the heads in `kBuiltinInterfaceHeads`, because nothing can
// occupy a concrete-typed slot but the struct.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_user_class_disposal_6402,
           owned_Array_field_cannot_hold_a_user_class_sealed_spec_12_4) {
    std::string src =
        "module M; "
        "class MyL: Array<int> "
        "  public constructor() end constructor; "
        "end MyL; "
        "class Holder "
        "  private owned _f: Array<int>; "
        "  public constructor() _f := new MyL(); end constructor; "
        "end Holder; "
        "class P public static method Main(): int return 0; end Main; end P;";
    auto r = bezel::test::runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    // §12.4 / §31.4: `Array` is closed to inheritance.
    bool sawClosed = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot inherit from compiler-known class") !=
                std::string::npos &&
            d.message.find("closed to inheritance") != std::string::npos)
            sawClosed = true;
    BEZEL_EXPECT_TRUE(sawClosed);
}