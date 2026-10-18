// Codegen — §8.1 / §28.5 / §28.7 / §29.7, v0.338 (#6414): an `owned`
// IEnumerable<int> FIELD holding a USER CLASS WILD-JUMPS. A different arm and a
// different head family from #6402 / #6413 / #6415 — the failure is a vtable
// SLOT read and an indirect CALL, not an array-layout mis-free.
//
// ROOT CAUSE, TWO SITES. Both classify an `owned IEnumerable<T>` field as
// `iterField` and then assume the value has the ITERATOR runtime format
// ([0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current — §29.7), loading slot 1 and
// calling it:
//
//   * src/codegen/LlvmBackend_Disposal.cpp, emitOwnedFieldDisposals, the
//     `it->iterField` arm — the scope-end release.
//   * src/codegen/LlvmBackend_Setup.cpp, lowerOwnedClassFieldStore — the
//     drop-on-overwrite release, required by §28.7.
//
// MEASURED that all THREE runtime kinds reach the scope-end arm, by
// instrumenting the classification: an array-backed `IEnumerable<int>` field,
// a real enumerator frame, and a user class all classify as `iterField`. The
// first TWO are correct today — the construction path materialises an
// array-backed `IEnumerable<int>` into a vtable-backed iterator WRAPPER, so its
// word 0 really is an iterator vtable whose slot 1 is a Dispose. The third is
// stored RAW, so word 0 is the CLASS vtable and slot 1 of it is not a Dispose
// function.
//
// §8.1 makes `class MyE: IEnumerable<int>` legal, and §28.5 admits the
// interface-typed owned field precisely because "runtime dispatch resolves
// disposal to the underlying class's Dispose(); an `owned IConnection` field
// disposes whichever class implementation it currently holds" — so sema accepts
// the store with NO diagnostic. MEASURED, 500 constructions / reassignments per
// row: `AddressSanitizer: SEGV … "PC is at a non-executable region. Maybe a wild
// jump?" #0 itmap$M.MyE`, with NO intervening stack frame (the slot-1 target is
// not a real function). Plain oracle: `Segmentation fault (core dumped)`,
// exit 139. This is a WILD JUMP, not a leak and not a bounded over-release.
//
// THE DISCRIMINATOR is `bz_obj_is_string(val, vtableTablePtr())` — the same
// helper and the same emitted vtable table #417 uses, and the same one #6402
// (scope-end list field), #6413 (drop-on-overwrite list field) and #6415 (sync
// `owned` list-head parameter) use.
//
// WHAT MAKES IT WORK HERE, and it is the opposite direction from those three, so
// it is worth stating rather than assuming: ITERATOR vtables are NOT REGISTERED,
// so they answer "string" and KEEP the slot-1 dispatch they need. In #6402's
// note an unregistered vtable is a CAVEAT ("still answers 'string' and takes the
// array arm — but such a value was already wild-freed before this change"); at
// this arm the same property is LOAD-BEARING, because taking the wrong arm here
// means calling MoveNext instead of Dispose and leaking the frame. A user
// class's vtable IS registered (user and imported classes alike, with #1843's
// `finalizeVtableTable` covering lazily-created specialized vtables), so it
// answers "not a string" and takes the class arm.
//
// The two CONTROL rows are what turn that argument into a measurement: they are
// the array-backed wrapper and the real enumerator, and both must stay on the
// slot-1 path. If the iterator vtables were registered after all, those two rows
// would take the class arm, skip the frame's own Dispose, and leak — so they are
// the evidence that the discriminator does not over-reach, not a formality.
//
// The class arm is `emitDisposeAndReclaim` — LITERALLY the class-instance
// release a user class in an `owned` slot gets everywhere else — so the class is
// freed exactly once and its own `owned` fields are discharged by its
// destructor rather than reinterpreted as an iterator frame.
//
// WHY EVERY ROW ARMS THE LEAK ORACLE (`LeakEnv("strict")`). The defect is a
// wild jump, but a fix that merely stopped jumping could do it by DECLINING the
// slot-1 dispatch — which would leak the enumerator frame instead. That is the
// same failure mode the controls are aimed at from the other side, and it is
// invisible to a plain run: `runAotChecked` DEFAULTS to
// `ASAN_OPTIONS=detect_leaks=0`, so a tree that stopped jumping by leaking would
// go green. Arming the knob is what makes "and does not leak" real. Each row
// accumulates 500–1000 heap objects, far above the 50–500 one-shot band, and
// every row returns a value derived from those iterations so a partial discharge
// also shows as a wrong exit code.
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

// `MyE` is a genuine §8.1 implementer, not a marker: the `owned _a: Array<int>`
// means a correct release has to reach through the destructor and free THAT too,
// so a release that frees `MyE` without cascading leaks. `MyE` is private, so
// its vtable has internal linkage — the #1843 registration case that
// `finalizeVtableTable` exists for.
constexpr const char* kMyE =
    "class MyE: IEnumerable<int> "
    "  private owned _a: Array<int>; "
    "  public constructor() _a := new Array<int>(1); end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return _a.GetEnumerator(); "
    "  end GetEnumerator; "
    "end MyE; ";

}  // namespace

// §8.1 / §28.5 / §29.7 (#6414) — THE HEADLINE SHAPE, the issue's own repro, at
// the SCOPE-END site. The field's declared type is an iterator-interface head
// and the runtime value is `MyE`, a vtable-bearing user class. Post-fix the
// class is released through its own Dispose + reclaim, which frees `MyE` and its
// `owned _a: Array<int>`.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_iterator_iface_field_user_class_disposal_6414,
           owned_IEnumerable_field_user_class_disposed_spec_8_1) {
    std::string src =
        std::string(
            "module M; ") + kMyE +
        "class Holder "
        "  private owned _f: IEnumerable<int>; "
        "  public constructor() _f := new MyE(); end constructor; "
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

// §28.7 / §29.7 (#6414) — the DROP-ON-OVERWRITE twin, in
// `lowerOwnedClassFieldStore`. §28.7 requires the displaced value to be
// disposed "null-guarded and dynamically dispatched, exactly the machinery
// scope-end disposal already uses", so both sites must classify the same value
// the same way; a fix at only one of them would leave this one jumping into a
// class vtable. The constructor's store is the initial one (the slot is still
// null, so nothing is disposed); `Reset`'s is the displacement.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_iterator_iface_field_user_class_disposal_6414,
           owned_IEnumerable_field_reassigned_user_class_disposed_28_7) {
    std::string src =
        std::string(
            "module M; ") + kMyE +
        "class Holder "
        "  private owned _f: IEnumerable<int>; "
        "  public constructor() _f := new MyE(); end constructor; "
        "  public method Reset() _f := new MyE(); end Reset; "
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

// §29.7 (#6414) — CONTROL 1, the vtable-backed iterator WRAPPER. This is the row
// that measures the load-bearing premise of the discriminator: this value's
// word 0 is an ITERATOR vtable, which is NOT registered, so it answers "string"
// and must keep the slot-1 dispatch. If the premise were wrong — if iterator
// vtables were in the table — this row would take the class arm, skip the
// frame's own Dispose (which frees the frame, §29.7), and leak. Green on both
// legs, which is the point.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_iterator_iface_field_user_class_disposal_6414,
           owned_IEnumerable_field_wrapper_still_slot1_spec_29_7) {
    std::string src =
        std::string(
            "module M; "
            "class Holder "
            "  private owned _f: IEnumerable<int>; "
            "  public constructor() _f := new Array<int>(1); end constructor; "
            "end Holder; "
            "class P public static method Main(): int "
            "  var t := 0; "
            "  for var i in 0..<500 do "
            "    var h := new Holder(); "
            "    t := t + 1; "
            "  end for; "
            "  return t mod 251; "
            "end Main; end P;");
    expectClean(src, 249);
}

// §29.7 (#6414) — CONTROL 2, a REAL enumerator frame, the shape the `iterField`
// arm was built for in the first place (#1643). Its word 0 is the state
// machine's own vtable; slot 1 is the Dispose that runs pending cleanups and
// FREES THE FRAME ITSELF. Calling the class arm here would leak the frame, so
// this row must stay on slot 1. Green on both legs.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_iterator_iface_field_user_class_disposal_6414,
           owned_IEnumerable_field_enumerator_frame_still_slot1_29_7) {
    std::string src =
        std::string(
            "module M; "
            "class Holder "
            "  private owned _a: Array<int>; "
            "  private owned _f: IEnumerable<int>; "
            "  public constructor() "
            "    _a := new Array<int>(1); "
            "    _f := _a.GetEnumerator(); "
            "  end constructor; "
            "end Holder; "
            "class P public static method Main(): int "
            "  var t := 0; "
            "  for var i in 0..<500 do "
            "    var h := new Holder(); "
            "    t := t + 1; "
            "  end for; "
            "  return t mod 251; "
            "end Main; end P;");
    expectClean(src, 249);
}

// §29.7 (#6414) — CONTROL 3, a LinkedList's OWN enumerator frame, the third
// iterator-collection runtime kind. Its word 0 is its own state-machine vtable,
// NOT registered, so it must keep the slot-1 path. Green on both legs, which
// demonstrates the discriminator handles another concrete collection's iterator
// separately from Array's wrapper.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_owned_iterator_iface_field_user_class_disposal_6414,
           owned_IEnumerable_field_linkedlist_enumerator_still_slot1_29_7) {
    std::string src =
        std::string(
            "module M; "
            "class Holder "
            "  private owned _l: LinkedList<int>; "
            "  private owned _f: IEnumerable<int>; "
            "  public constructor() "
            "    _l := new LinkedList<int>(); "
            "    _f := _l.GetEnumerator(); "
            "  end constructor; "
            "end Holder; "
            "class P public static method Main(): int "
            "  var t := 0; "
            "  for var i in 0..<500 do "
            "    var h := new Holder(); "
            "    t := t + 1; "
            "  end for; "
            "  return t mod 251; "
            "end Main; end P;");
    expectClean(src, 249);
}

// §28.8 (#6414) — CONTROL 4, and the one that CORRECTED this file's first
// draft. It was written as an over-release guard: `Reset` doing `_f := _f` on
// an `owned` field, which `lowerOwnedClassFieldStore`'s identity guard
// (`oldVal != newVal`) exists to make a no-op. MEASURED: that program does not
// compile at all — reading an `owned` field produces a BORROW, and sema rejects
// storing a borrow back into an `owned` field, because the field's owner would
// dispose a value owned elsewhere:
//
//   a borrow cannot be stored into an `owned` field: the field's owner disposes
//   the value when the object is disposed, but a borrow is owned elsewhere and
//   its real owner disposes it too — a double-free (§28.3 / §28.5 / §28.8 …)
//
// So the identity guard is unreachable for this shape on both legs, and the
// double-free it guards against is caught STATICALLY rather than at run time.
// That is worth pinning: it is a compiler-enforced invariant (Appendix C) at the
// exact site whose new branch reads `oldVal`, and a future change that widened
// the owned-field read to produce an owned value would turn this row red.
//
// Uses the plain `runAot` oracle: this row asserts a REJECTION, so there is no
// program to run and no leak oracle to arm.
BEZEL_TEST(codegen_owned_iterator_iface_field_user_class_disposal_6414,
           owned_IEnumerable_field_self_assign_is_a_static_double_free_28_8) {
    std::string src =
        std::string(
            "module M; ") + kMyE +
        "class Holder "
        "  private owned _f: IEnumerable<int>; "
        "  public constructor() _f := new MyE(); end constructor; "
        "  public method Reset() _f := _f; end Reset; "
        "end Holder; "
        "class P public static method Main(): int return 0; end Main; end P;";
    auto r = bezel::test::runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawBorrowDiagnostic = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("a borrow cannot be stored into an `owned` field") !=
                std::string::npos)
            sawBorrowDiagnostic = true;
    BEZEL_EXPECT_TRUE(sawBorrowDiagnostic);
}
