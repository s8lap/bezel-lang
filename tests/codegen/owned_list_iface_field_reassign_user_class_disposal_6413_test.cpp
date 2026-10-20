// Codegen — §28.7 / §28.5 / §12.4, v0.338 (#6413): REASSIGNING a user class
// into an `owned` list-interface-head FIELD wild-frees — the drop-on-OVERWRITE
// twin of #6402's scope-end field site.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp,
// `lowerOwnedCollectionFieldStore`). The store that replaces what an `owned`
// list-head field already holds loaded the displaced pointer (`oldVal`) and
// emitted the free that its DECLARED type implies: `bz_array_free_dynamic` for a
// borrowing head with no derivable element disposer, `bz_array_free(val, disp)`
// otherwise. That is correct for every CONCRETE built-in name, because §12.4
// closes those to inheritance — `class MyL: Array<int>` is rejected outright:
//
//   cannot inherit from compiler-known class `Array`: it is closed to
//   inheritance, as a `sealed` class is … (§12.4, §31.4)
//
// It is NOT correct for the LIST HEADS. §28.5 states the rule this arm was
// missing: "Interface-typed owned fields are admitted because runtime dispatch
// resolves disposal to the underlying class's Dispose(); an `owned IConnection`
// field disposes whichever class implementation it currently holds." `IList<T>`
// IS an interface type, so `class MyL: IList<int>` is legal and sema accepts
// storing one into `owned _f: IList<int>` with no diagnostic. On such a value
// word 0 is a VTABLE POINTER, so `arr->data` / `arr->count` /
// `arr->elem_disposer` are read OFF THE VTABLE — a WILD FREE, not a leak.
//
// §28.7 is the rule the fix restores, and it is worth quoting because it says
// the site must not have had its own idea of the value's kind in the first
// place: "The disposal is null-guarded and dynamically dispatched, exactly the
// machinery scope-end disposal already uses." This IS that drop-on-overwrite
// site, so its disposal must be the same dynamic dispatch the scope-end site
// uses.
//
// WHY A RUNTIME DISCRIMINATION AND NOT #6368's NARROWING. #6368's async-capture
// fix-forward declined the list heads, on the grounds that the static type does
// not determine the runtime kind. That narrowing was MEASURED UNSAFE at #6402's
// scope-end field site: it declined the free on array-backed list-interface
// `owned` fields across 30 rows in five suites
// (`..._2603`, `..._2606`, `..._2608`, `..._2635`, `..._2630`), turning a clean
// run into a leak. The same array-backed spellings reach THIS site — the third
// control row below is exactly one — so narrowing here would trade a crash for
// the same leak. The fix therefore discriminates at RUN TIME with
// `bz_obj_is_string(val, vtableTablePtr())`, the same helper and the same
// emitted vtable table #417 uses and the same one #6402 uses at the scope-end
// twin.
//
// WHY EVERY ROW IS A REAL LEAK ORACLE. The defect is a wild free, but the fix
// MOVES a release: before it, a user class in this slot was accidentally
// "released" by the array free; after it, the release is `emitDisposeAndReclaim`,
// which must free the user class AND let its own `owned` fields die with it. A
// fix that merely stopped crashing by declining the free would leak, and a row
// that could not see that leak would go green on exactly that broken tree.
// `runAotChecked` passes `/*detectLeaks=*/true` as a hardcoded argument
// (aot_support.hpp), so the harness arms LeakSanitizer BY CONSTRUCTION and the
// "and does not leak" half is real — the `LeakEnv("strict")` scope below is
// NOT what arms it. Each row
//
// WHY `LeakEnv("strict")` IS STILL HERE even though it is not what makes these
// rows observe a leak. The `BEZEL_AOT_DETECT_LEAKS` knob is read at
// aot_support.hpp:3416 and :4014, which are `runAot` and the two-module path —
// NOT the `runAotChecked` family — so for these rows it is belt-and-braces
// rather than load-bearing. It is kept for the direction it DOES protect: the
// gate may run a whole shard with the knob set, and the RAII save/restore
// stops that from polluting a `runAot`-based row elsewhere in the same process.
// It is deliberately NOT what the leak rows rest on.
//
// Each row
// accumulates 500 distinct `MyL`s (1000 on most rows — the constructor's store
// plus `Reset`'s), far above the 50–500 one-shot band, and every row returns a
// value derived from those iterations so a partial discharge shows as a wrong
// exit code too.
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
//
// IT IS INERT for every `runAotChecked` row in this file: `runAotCheckedImpl`
// takes `detectLeaks` as a hardcoded argument and never consults this
// variable — `leakDetectMode()` is read only by `runAot` — so this scope
// changes nothing about the leak oracle. Do NOT read it as "this row's leak
// checking is armed here": it is `runAotChecked` vs `runAotCheckedLenient`
// that decides that.
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

// The reassignment that reaches the drop-on-overwrite arm. `_f := new MyL()`
// as the constructor's first store is the INITIAL store (the slot is still the
// calloc'd null, so `ocf.live` is false and nothing is disposed); it is the
// SECOND store, in `Reset`, that displaces a live MyL and is where the wild
// free happened. `self-assign` and `null` rows are covered separately.
constexpr const char* kReassignTail =
    "  public method Reset() _f := new MyL(); end Reset; "
    "end Holder; "
    "class P public static method Main(): int "
    "  var t := 0; "
    "  for var i in 0..<500 do "
    "    var h := new Holder(); "
    "    h.Reset(); "
    "    t := t + 1; "
    "  end for; "
    "  return t mod 251; "
    "end Main; end P;";

}  // namespace

// §28.7 (#6413) — THE HEADLINE SHAPE, the issue's own repro. The field's
// declared type is a borrowing list head and the DISPLACED runtime value is
// `MyL`, a vtable-bearing user class. Pre-fix this SEGVs in
// `bz_array_free_dynamic` (no element disposer is derivable for an `int`
// element, so the #2608 dynamic branch is taken). Post-fix the class is
// disposed through its own vtable, which frees `MyL` and its
// `owned _a: Array<int>` — LSan-clean.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_reassign_user_class_disposal_6413,
           owned_IList_field_reassigned_user_class_disposed_spec_28_7) {
    std::string src =
        std::string("module M; ") + kIListImpl +
        "class Holder "
        "  private owned _f: IList<int>; "
        "  public constructor() _f := new MyL(); end constructor; "
        + kReassignTail;
    expectClean(src, 249);
}

// §28.7 (#6413) — the READ-ONLY head. Same defect, second spelling: it is in
// `kBuiltinInterfaceHeads` for the same reason and reaches the same #2608
// dynamic branch. Pinned separately so a fix that special-cased `IList` alone
// would leave this red.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_reassign_user_class_disposal_6413,
           owned_IReadOnlyList_field_reassigned_user_class_disposed_28_7) {
    std::string src =
        std::string("module M; ") +
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
        + kReassignTail;
    expectClean(src, 249);
}

// §28.7 / §28.11 (#6413) — the OWNING head, which takes a DIFFERENT emit path
// inside the same arm: `collIsOwning` is set, so this reaches the plain
// `bz_array_free(val, disp)` rather than the dynamic branch. Pinned separately
// for that reason — a fix that only guarded the `bz_array_free_dynamic` arm
// would leave this one wild-free.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_reassign_user_class_disposal_6413,
           owned_IOwningList_field_reassigned_user_class_no_cascade_28_11) {
    std::string src =
        std::string("module M; ") +
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
        + kReassignTail;
    expectClean(src, 249);
}

// §28.7 / §28.9 (#6413) — the STRING-element head, which takes a THIRD path:
// a `string` element derives a live disposer, so the `!elemDisp` guard above is
// false and the emit goes to `bz_array_free(val, disp)` with a non-null
// disposer. Without this row a fix could guard the dynamic branch and the
// `collIsOwning` shape and still leave this spelling wild-free.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_reassign_user_class_disposal_6413,
           owned_IList_string_elem_reassigned_user_class_disposed_28_9) {
    std::string src =
        std::string("module M; ") +
        "class MyL: IList<string> "
        "  private owned _a: Array<string>; "
        "  public constructor() _a := new Array<string>(); "
        "    _a.Add(\"hello\"); "
        "  end constructor; "
        "  public method GetEnumerator(): IEnumerator<string> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end MyL; "
        "class Holder "
        "  private owned _f: IList<string>; "
        "  public constructor() _f := new MyL(); end constructor; "
        + kReassignTail;
    expectClean(src, 249);
}

// §28.7 (#6413) — the MIXED-KIND row, and the one that pins the discriminator
// as a PER-EXECUTION decision on the displaced value rather than a per-FIELD one.
// ONE field, ONE declared type (`IList<int>`), THREE stores, so the drop-on-
// overwrite arm runs twice with the two different runtime kinds:
//
//   ctor  : _f := new Array<int>(3)   initial store (slot still null — disposes
//                                      nothing; this is the `ocf.live` arm)
//   Mid   : _f := new MyL()           displaces an ARRAY      -> array arm
//   Reset : _f := new Array<int>(4)   displaces a USER CLASS -> vtable arm
//
// So the field holds a user class at exactly one point, and the arm must take
// the class arm there while taking the array arm one step earlier. A fix that
// classified the SLOT once (from the declared type, or by caching the first
// observed value) would get one of the two displacements wrong: caching the
// array would wild-free `MyL` on `Reset`, caching the class would leak the
// backing array on `Mid`.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_reassign_user_class_disposal_6413,
           owned_IList_field_reassign_mixed_kinds_per_execution_spec_28_7) {
    std::string src =
        std::string("module M; ") + kIListImpl +
        "class Holder "
        "  private owned _f: IList<int>; "
        "  public constructor() _f := new Array<int>(3); end constructor; "
        "  public method Mid() _f := new MyL(); end Mid; "
        "  public method Reset() _f := new Array<int>(4); end Reset; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    h.Mid(); "
        "    h.Reset(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §28.11 (#6413) — CONTROL 1, the array-backed half of `IList` under
// REASSIGNMENT. The displaced runtime value really is a `bz_array_t*`, so the
// correct release really IS `bz_array_free`, and the discriminator must keep
// taking the array arm. This row is GREEN BEFORE THE FIX AND AFTER IT, which is
// the point: it is what rules out the "narrow the static type" fix #6368 used
// at the capture end, at THIS site. Narrowing would turn this program into a
// leak of 500 array backings.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_reassign_user_class_disposal_6413,
           owned_IList_field_reassign_array_backing_still_freed_28_11) {
    std::string src =
        std::string("module M; ") +
        "class Holder "
        "  private owned _f: IList<int>; "
        "  public constructor() _f := new Array<int>(3); end constructor; "
        "  public method Reset() _f := new Array<int>(4); end Reset; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    h.Reset(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §28.7 (#6413) — CONTROL 2, the CONCRETE-name half of the sealed-class
// argument, under reassignment. §12.4 closes `Array<int>` to inheritance, so a
// user class can never occupy this slot and the discriminator's `live` arm is
// unreachable here. This row establishes that the fix's premise is narrow: the
// runtime check is needed for the heads in `kBuiltinInterfaceHeads`, because
// nothing can occupy a concrete-typed slot but the struct itself.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_list_iface_field_reassign_user_class_disposal_6413,
           owned_Array_field_reassign_of_concrete_backing_spec_28_11) {
    std::string src =
        std::string("module M; ") +
        "class Holder "
        "  private owned _f: Array<int>; "
        "  public constructor() _f := new Array<int>(3); end constructor; "
        "  public method Reset() _f := new Array<int>(4); end Reset; "
        "end Holder; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var h := new Holder(); "
        "    h.Reset(); "
        "    t := t + 1; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectClean(src, 249);
}

// §12.4 / §31.4 (#6413) — CONTROL 3, the same sealed-class argument as a
// compile-time rejection: `class MyL: Array<int>` is refused, so the
// concrete-typed `owned` field can never receive a user class. This is the
// premise the runtime discriminator's soundness rests on, asserted directly so
// a future change that OPENS `Array` to inheritance turns this row red and the
// discriminator has to be re-derived rather than silently over-trusting.
//
// (Uses the plain `runAot` oracle: this row asserts a REJECTION, so there is
// no program to run and no leak oracle to arm.)
BEZEL_TEST(codegen_owned_list_iface_field_reassign_user_class_disposal_6413,
           owned_Array_field_cannot_hold_a_user_class_sealed_spec_12_4) {
    std::string src =
        std::string("module M; ") +
        "class MyL: Array<int> "
        "  public constructor() end constructor; "
        "end MyL; "
        "class Holder "
        "  private owned _f: Array<int>; "
        "  public constructor() _f := new MyL(); end constructor; "
        "  public method Reset() _f := new MyL(); end Reset; "
        "end Holder; "
        "class P public static method Main(): int return 0; end Main; end P;";
    auto r = bezel::test::runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawClosed = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot inherit from compiler-known class") !=
                std::string::npos &&
            d.message.find("closed to inheritance") != std::string::npos)
            sawClosed = true;
    BEZEL_EXPECT_TRUE(sawClosed);
}