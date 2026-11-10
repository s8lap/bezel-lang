// Codegen — #8215 (§28.9 / §8.6, v0.357): a box field whose class also stores
// an `Object`-typed value leaked every box it held.
//
// THE DEFECT. `Emitter::collectBoxStorageFields` (src/codegen/
// LlvmBackend_Setup.cpp) classifies an `Object`/interface-typed class field as
// "box-only" iff EVERY write to it puts a §8.6 box (or null) in the slot, and
// only a box-only field's disposer calls `bz_box_release`. After 00aab520
// (narrow #7968) a store of an `Object`-typed PARAMETER counts as a non-box
// store, so a class with
//
//     public method Take(n: int) o := n; end Take;
//     public method Put(x: Object) o := x; end Put;
//
// has an EXCLUDED field `o`: `Take` gives it a +1 box (lowerBoxFieldStore),
// and nothing ever releases it -- the disposer skips the field, and a store
// that displaces a box skips the release-old. MEASURED on the base (issue
// #8215, h23: `Main` calls only `Take`, 100 Holders): 2400 B in 100 objects.
//
// THE FIX. The slot's occupant cannot be known statically, so for such a field
// (it receives a box AND something else, every write being one the store sites
// handle) it is told apart at RUN TIME, with the discriminator the `owned
// Object` discharge already uses (#8118; `emitObjectSlotBoxOp`): a box is the
// one occupant that is not null, not a string, not a function value, not a
// tagged built-in, whose vtable's Dispose slot is the shared box no-op and
// which is not the `Object` root. Every store gives the field a +1 on a box
// and ignores anything else (so an `Object` parameter that holds a box is
// RETAINED by the field -- §28.9: a box is "an immutable shared reference"),
// and the displaced value and the disposer release a box and ignore anything
// else. A class instance, a string or a function value is exactly as it was.
//
// ORACLE. `runAotChecked` links the program under ASan with LeakSanitizer
// armed, so a leak (the defect) or a double release / use-after-free (the
// trade a wrong fix would make) turns into `ran == false`. Every program loops
// the shape 100 times: a one-shot leak can hide under the sanitizer's noise.
// Rows that need a VALUE read it back, so a fix that dropped the store or
// released a live box fails.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Row 1 -- the issue's repro (h23): `Main` calls only `Take`.
const char* kIssueRepro =
    "module M8215A; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "  public method Take(n: int) o := n; end Take; "
    "  public method Put(x: Object) o := x; end Put; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    for var i in 0..<100 do "
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "    end for; "
    "    Console.WriteLine(\"ok\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Row 2 -- a box displaced by a class instance, then a box again, then a
// class instance: the displaced boxes are released at the store, the class
// instance is never touched (it prints its field), the last occupant is a
// class instance owned elsewhere.
const char* kBoxThenClassThenBox =
    "module M8215B; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "  public method Take(n: int) o := n; end Take; "
    "  public method Put(x: Object) o := x; end Put; "
    "end Holder; "
    "public class Widget "
    "  public v: int; "
    "  public constructor(v: int) this.v := v; end constructor; "
    "  public method Get(): int => v; "
    "end Widget; "
    "public class Program "
    "  public static method Main(): int "
    "    using var w := new Widget(7); "
    "    for var i in 0..<100 do "
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      h.Put(w); "
    "      h.Take(i + 1); "
    "      h.Put(w); "
    "    end for; "
    "    Console.WriteLine(w.Get()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Row 3 -- an `Object`-typed SOURCE that holds a box (another holder's field)
// is stored by `CopyFrom`: the destination field takes its OWN +1 (the source
// keeps its own), so the box is released exactly once by each holder. Without
// the retain both holders would release the one reference (an over-release
// the sanitizer reports); without the release the box leaks. The values read
// back are the stored ones: 2 * (0 + ... + 99) = 9900.
//
// (The shape is not spelled `Put(b)` of a boxed `Object` LOCAL or argument: a
// box a CALLER passes to a call is not released by the caller whatever the
// callee does with it -- the separate leak recorded in the handback -- so such a
// row would be red for a reason that is not this issue's.)
const char* kObjectSourceHoldingBox =
    "module M8215C; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "  public method Take(n: int) o := n; end Take; "
    "  public method CopyFrom(other: Holder) o := other.o; end CopyFrom; "
    "  public method Read(): int "
    "    return (o as int) ?? -1; "
    "  end Read; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    var sum := 0; "
    "    for var i in 0..<100 do "
    "      using var a := new Holder(); "
    "      using var b := new Holder(); "
    "      a.Take(i); "
    "      b.CopyFrom(a); "
    "      sum += b.Read() + a.Read(); "
    "    end for; "
    "    Console.WriteLine(sum); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Row 4 -- the same shape through an auto-property setter.
const char* kAutoProperty =
    "module M8215D; "
    "public class Holder "
    "  public property O: Object? := null; "
    "  public constructor() end constructor; "
    "  public method Take(n: int) O := n; end Take; "
    "  public method Put(x: Object) O := x; end Put; "
    "end Holder; "
    "public class Widget "
    "  public v: int; "
    "  public constructor(v: int) this.v := v; end constructor; "
    "  public method Get(): int => v; "
    "end Widget; "
    "public class Program "
    "  public static method Main(): int "
    "    using var w := new Widget(7); "
    "    for var i in 0..<100 do "
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      h.Put(w); "
    "      h.Take(i); "
    "    end for; "
    "    Console.WriteLine(w.Get()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Row 5 -- CONTROL: a field that only ever receives boxes keeps the static
// (box-only) discipline and stays clean.
const char* kBoxOnlyControl =
    "module M8215E; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "  public method Take(n: int) o := n; end Take; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    for var i in 0..<100 do "
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      h.Take(i + 1); "
    "    end for; "
    "    Console.WriteLine(\"ok\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// FAIL-WITHOUT: LeakSanitizer reports 2400 B in 100 objects, so `ran` is false.
BEZEL_TEST(codegen_box_field_dynamic_release_8215,
           spec_28_9_8_6_issue_repro_take_beside_put_leaks_nothing) {
    JitResult r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// FAIL-WITHOUT: the displaced and the final box leak.
BEZEL_TEST(codegen_box_field_dynamic_release_8215,
           spec_28_9_8_6_box_class_box_class_overwrites_release_only_boxes) {
    JitResult r = runAotChecked(kBoxThenClassThenBox);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// FAIL-WITHOUT: the boxes `Take` stores leak (and a fix that released without
// retaining would over-release). The sum is 2 * (0 + ... + 99).
BEZEL_TEST(codegen_box_field_dynamic_release_8215,
           spec_28_9_8_6_object_source_holding_box_is_retained_not_stolen) {
    JitResult r = runAotChecked(kObjectSourceHoldingBox);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9900\n");
}

// FAIL-WITHOUT: the boxes the property setter stores leak.
BEZEL_TEST(codegen_box_field_dynamic_release_8215,
           spec_28_9_8_6_auto_property_setter_releases_boxes_only) {
    JitResult r = runAotChecked(kAutoProperty);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// CONTROL (green on the base too): a box-only field is unchanged.
BEZEL_TEST(codegen_box_field_dynamic_release_8215,
           spec_28_9_8_6_box_only_field_control_unchanged) {
    JitResult r = runAotChecked(kBoxOnlyControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}
