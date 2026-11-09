// Codegen — #8344 (§8.6 / §28.9, v0.355): a box store into an `Object` field
// released the DISPLACED OLD VALUE as a box, without ever asking what the
// old value was.
//
// THE DEFECT.
//
// `Emitter::lowerBoxFieldStore` (src/codegen/LlvmBackend_Setup.cpp) lowers a
// store of a boxable value into a class `Object`/interface field as
//
//     load-old → transfer/retain-new → store-new → release-old
//
// and the `release-old` step was UNGATED: the displaced value's KIND was
// never checked. `isBoxFieldLvalue` — the gate above it — classifies the NEW
// source's type ("is the value being stored a boxable value type?"), which
// says nothing about the value that was ALREADY in the slot. The slot's
// occupant is the DISPLACED value, and its kind comes from whatever put it
// there: the field's INITIALIZER, an EARLIER store, a constructor, a lambda
// body, a destructor body.
//
// §8.6 (docs/Bezel_Language_Spec.md:1228) makes the consequence a SPEC
// violation, not merely a crash: "a class instance, a string, a heap array,
// and a function type are reference types whose values are already
// heap-allocated" and "reach `Object` directly through the inheritance
// chain" — a STRING IN AN `Object` SLOT IS NOT A BOX, and a CLASS INSTANCE IN
// AN `Object` SLOT IS NOT A BOX. §28.9 (docs/Bezel_Language_Spec.md:8112)
// adds that `string` and boxed value types are RUNTIME-MANAGED, so neither is
// released by `bz_box_release`.
//
// MEASURED on the base (r30-c1, 8c8aef19), under the generated-code ASan:
//
//   * `public O: Object := "";` then `h.O := 3;`
//       → "SUMMARY: AddressSanitizer: SEGV
//          build/../src/runtime/bezel_runtime_refcounting.cpp:757:25 in
//          bz_box_release"  (the displaced STRING released as a box: a write
//          through a non-box pointer)
//   * `h.O := "runtime"; h.O := 3;` → the same SEGV
//   * a field holding a USER CLASS INSTANCE that is then overwritten by a box
//       → the class instance's word at offset 8 (its FIRST FIELD, not a
//         refcount) is decremented as if it were one: the program prints `6`
//         where `7` is expected.
//
// THE FIX.
//
// The displaced value's kind is established STATICALLY, at the producer/store
// site — the runtime deliberately does NOT inspect the vtable to tell a box
// from a class instance (the SAFETY note in
// src/runtime/bezel_runtime_refcounting.cpp says the distinction "is resolved
// at the codegen producer/store site"). The verdict is the `collectBoxStorageFields`
// pre-pass's, which is a JOIN over EVERY write to the field (its initializer
// plus every store in every body the pre-pass reaches): the field is a
// box-storage field iff EVERY one of those writes puts a BOX (or null) in the
// slot. A join over all writes implies the per-store fact the store site
// needs, so the `release-old` step is emitted only for a field whose slot can
// only ever hold a box.
//
// THE LEAK DIRECTION IS EXPLICIT, NOT A COVER-UP. A field the pre-pass
// EXCLUDED (one that receives a class-reference / string / unknown-provenance
// store somewhere in the program) may hold such a value at THIS store site, so
// the displaced value is NOT released there: a LEAK, never a crash. That is
// the same conservative direction the LOCAL machinery already takes for the
// same question (`boxLocalAssignmentSourceKind` returns -1 for
// "Object/interface provenance could already be either", and its own comment
// says "Unknown provenance stays conservatively unmanaged; a leak is safer
// than handing a class instance to `bz_box_release`"). Rows 5 and 6 below
// pin that the DISPLACED BOX of a box-only field is STILL RELEASED — the fix
// does not trade the crash for an unbounded leak.
//
// THE ORACLE. Two oracles, and WHY they differ.
//
// `runAotChecked` links the generated program under ASan with LeakSanitizer
// armed (`detect_leaks=1`), so a leak OR a sanitizer report turns into
// `ran == false`. It is the oracle for the rows whose subject is the LEAK
// (rows 5 and 6): a box-only field must leak nothing.
//
// `runAot` is the oracle for the rows whose subject is the CRASH (rows 1, 2
// and 7). Those rows exercise a MIXED field — one the pre-pass cannot admit
// as a box-storage field — and a mixed field's FINAL box is not released at
// disposal. That is the pre-existing #8215 leak ("a box field whose class
// also stores an Object-typed parameter leaks every box"), which this issue
// does NOT close (see the RESIDUAL note below); it is not a leak this change
// introduces, and it is not what these rows are about. `runAot` keeps
// LeakSanitizer off (so the known residual does not turn the row red) while
// still failing the row on the crash: the base's SEGV kills the program, and
// the harness reports "program terminated by signal N" (ran == false). Under
// the ASan sweep (`BEZEL_AOT_FORCE_SANITIZE=address`, which the gate runs)
// `runAot` instruments the generated program with AddressSanitizer too, so
// the base's crash is reported with its full ASan backtrace.
//
// The RESIDUAL, measured (base r30-c1 vs this fix, `bezelc --sanitize=address`
// with LeakSanitizer on):
//
//   shape                              base                    this fix
//   --------------------------------  ----------------------  ----------------------
//   `O: Object := ""` then `O := 3`   SEGV in bz_box_release  "3", 24 B / 1 alloc
//   `O := "runtime"` then `O := 3`    SEGV in bz_box_release  "3", 24 B / 1 alloc
//   a class instance, then `O := 3`   prints "6" (corrupted)   prints "7", 24 B / 1 alloc
//
// The 24 B is the one box the mixed field holds at disposal — the #8215
// residual, visible in the third row on the BASE as well (the base prints
// "6" and leaks the same 24 B). The first two rows crash on the base, so
// their leak is masked there; it is the same residual, not a new one.
//
// ROWS 5 AND 6 ARE THE LEAK ROWS, AND THEY ARE CLEAN: a box-only field
// releases every box it holds, at the store site and at disposal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ---------------------------------------------------------------------------
// Row 1 — the issue's repro 1: the field's INITIALIZER is a string literal.
//
// The initializer is not a "store" in any scanned body, but it does put a
// string in the slot, so the box store that follows displaces a STRING.
// MEASURED on the base: SEGV in bz_box_release.
//
// The row asserts the VALUE read back, not merely the absence of a crash: a
// fix that suppressed the store (or the read) would pass a crash-only oracle.
// ---------------------------------------------------------------------------
const char* kStringInitializerThenBox =
    "module M8344A; "
    "public class Holder "
    "  public O: Object := \"\"; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    h.O := 3; "
    "    Console.WriteLine(h.O.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 2 — the issue's repro 2: an EARLIER STRING store, then a box store.
//
// MEASURED on the base: the same SEGV in bz_box_release.
// ---------------------------------------------------------------------------
const char* kStringStoreThenBox =
    "module M8344B; "
    "public class Holder "
    "  public O: Object := \"\"; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    h.O := \"runtime\"; "
    "    h.O := 3; "
    "    Console.WriteLine(h.O.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 3 — CONTROL: a `null` initializer. The displaced value is null, and
// `bz_box_release` is null-safe, so this row is unchanged by the fix and
// pins that the fix did not change the null path.
// ---------------------------------------------------------------------------
const char* kNullInitializerControl =
    "module M8344C; "
    "public class Holder "
    "  public O: Object := null; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    h.O := 3; "
    "    Console.WriteLine(\"ok \" + h.O.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 4 — CONTROL: an `int` initializer. The displaced value is a BOX (the
// initializer boxed it), so the release still happens and still must.
// ---------------------------------------------------------------------------
const char* kIntInitializerControl =
    "module M8344D; "
    "public class Holder "
    "  public O: Object := 0; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    h.O := 3; "
    "    Console.WriteLine(h.O.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 5 — THE LEAK ROW (a box-only field): the displaced BOX must still be
// released.
//
// A field that only ever receives boxes holds a box at every point, so the
// store-side release is emitted and the displaced box is freed. 200
// iterations x 3 stores = 600 boxes minted; a suppressed release would leak
// 400 of them (24 B each). LSan-clean is the oracle.
//
// MEASURED on the base: LSan-clean. MEASURED with the fix: LSan-clean —
// the fix suppresses the release ONLY for a field whose slot can hold a
// non-box.
// ---------------------------------------------------------------------------
const char* kDisplacedBoxStillReleased =
    "module M8344E; "
    "public class Holder "
    "  public O: Object?; "
    "  public constructor() end constructor; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    for var i in 0..<200 do "
    "      using var h := new Holder(); "
    "      h.O := 1; "
    "      h.O := 2; "
    "      h.O := 3; "
    "    end for; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 6 — THE DISPOSAL ROW: a box-only field's box is released at scope end.
//
// §28.9: a boxed value type is runtime-managed, and the owner releases the
// field's independent +1 when the owner is disposed. This is the DISPOSER's
// release (emitDisposerCore's box-field loop), the twin of the store-side
// release the fix gates; the fix must not disarm it. 200 iterations, one box
// each — a suppressed release leaks 200 boxes.
//
// MEASURED on the base: LSan-clean. MEASURED with the fix: LSan-clean.
// ---------------------------------------------------------------------------
const char* kBoxOnlyFieldReleasedAtDisposal =
    "module M8344F; "
    "public class Holder "
    "  public O: Object?; "
    "  public constructor() end constructor; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    for var i in 0..<200 do "
    "      using var h := new Holder(); "
    "      h.O := 42; "
    "    end for; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// Row 7 — a CLASS INSTANCE displaced by a box store.
//
// The class instance's bytes are { vtable, i8 disposed-flag@1, fields }; the
// word at offset 8 is the instance's FIRST FIELD, not a refcount. Releasing
// it as a box decremented that field: the program printed `6` where `7` is
// expected. The store is made through a PARAMETER (a borrow), because §28.3
// rejects a scope-bound owned local stored into a field.
//
// MEASURED on the base: "6". MEASURED with the fix: "7".
// ---------------------------------------------------------------------------
const char* kClassInstanceDisplacedByBox =
    "module M8344G; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "end Holder; "
    "public class Widget "
    "  public v: int; "
    "  public constructor(v: int) this.v := v; end constructor; "
    "  public method Get(): int => v; "
    "end Widget; "
    "public class L "
    "  public static method Put(h: Holder, w: Widget) h.o := w; end Put; "
    "end L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    using var w := new Widget(7); "
    "    L.Put(h, w); "
    "    h.o := 3; "
    "    Console.WriteLine(w.Get()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — the field's initializer is a string; the box store must not
// release it as a box.
//
// FAIL-WITHOUT: on the base this SEGVs in bz_box_release, so `ran` is false.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_field_displaced_release_8344,
           spec_8_6_28_9_string_initializer_then_box_store_no_segv) {
    JitResult r = runAot(kStringInitializerThenBox);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// ---------------------------------------------------------------------------
// Row 2 — an earlier string store, then a box store.
//
// FAIL-WITHOUT: the same SEGV in bz_box_release.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_field_displaced_release_8344,
           spec_8_6_28_9_string_store_then_box_store_no_segv) {
    JitResult r = runAot(kStringStoreThenBox);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// ---------------------------------------------------------------------------
// Row 3 — CONTROL: the `null` initializer path is unchanged.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_field_displaced_release_8344,
           spec_8_6_null_initializer_control_unchanged) {
    JitResult r = runAotChecked(kNullInitializerControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok 3\n");
}

// ---------------------------------------------------------------------------
// Row 4 — CONTROL: the `int` initializer path is unchanged (the displaced
// value there IS a box, and it is still released).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_field_displaced_release_8344,
           spec_8_6_int_initializer_control_unchanged) {
    JitResult r = runAotChecked(kIntInitializerControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// ---------------------------------------------------------------------------
// Row 5 — THE LEAK ROW: the displaced BOX of a box-only field is still
// released. LSan-clean.
//
// A suppressed release here would leak 400 boxes (24 B each) over the 200
// iterations.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_field_displaced_release_8344,
           spec_28_9_displaced_box_of_box_only_field_is_still_released) {
    JitResult r = runAotChecked(kDisplacedBoxStillReleased);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}

// ---------------------------------------------------------------------------
// Row 6 — THE DISPOSAL ROW: a box-only field's box is released when its
// owner is disposed (§28.9). LSan-clean.
//
// A suppressed release here would leak 200 boxes (24 B each).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_field_displaced_release_8344,
           spec_28_9_box_only_field_released_at_owner_disposal) {
    JitResult r = runAotChecked(kBoxOnlyFieldReleasedAtDisposal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}

// ---------------------------------------------------------------------------
// Row 7 — a class instance displaced by a box store is not corrupted.
//
// FAIL-WITHOUT: on the base the class instance's first field is decremented
// as if it were a refcount, and the program prints "6" where "7" is
// expected.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_box_field_displaced_release_8344,
           spec_8_6_class_instance_displaced_by_box_is_not_corrupted) {
    JitResult r = runAot(kClassInstanceDisplacedByBox);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}
