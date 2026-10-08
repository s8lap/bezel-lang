// Codegen — #5105 / §8.6 / §28.2 / §28.9 (v0.315).
// When a record with string fields is boxed into a user-interface parameter
// (e.g. `IFoo`), the record's underlying storage leaks — the box conversion
// does not dispose the original record after boxing.
//
// This is a sibling of #4832 (the Equals-call case) but the leak occurs on
// box conversion itself, without needing an Equals call.
//
// THE REPRO. The issue was found by a non-author review of #5095 using a
// 200-iteration loop with ASan. The measured values:
//   Direct leak: 6400 bytes in 200 objects (the box — {vtable*, i64 rc, payload})
//   Indirect leak: 890 bytes in 200 objects (the strings — bz_string headers)
//   SUMMARY: 7290 bytes leaked in 400 allocations.
//
// THE FIX. After `boxForTarget` mints a fresh box in `buildOrderedArgs`, the
// original record value (which was loaded from the local slot and then passed
// to boxForTarget) must be disposed. The box owns the record's contents at
// this point; the original record struct is no longer needed. Without this
// disposal, the record's string fields are never released and the 24-byte
// box struct itself also leaks.
//
// THE TESTS:
//
// T1 — THE PIN. String-field record through user interface parameter with
//   no Equals call. 200 iterations amplify the leak so LSan detects it
//   decisively. The program prints 200 (correct output); LSan clean is
//   the only discriminator.
//
// T2 — Same shape with Object parameter (not just user interface).
//   The same boxing-path leak occurs per §8.6.
//
// T3 — Control: int-field record through interface — the existing #2952
//   mechanism already releases the box; LSan stays clean.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// T1 — THE PIN. Record with string fields boxed to user interface parameter.
// LSan: Direct 6400 B (200×24B box) + Indirect 890 B (200×string headers).
const char* kStringFieldRecordViaInterface =
    "module M5105A; "
    "interface IFoo5105A "
    "  method Get(): int; "
    "end IFoo5105A; "
    "record Name5105A(A: string, B: string): IFoo5105A "
    "  public method Get(): int => 1; "
    "end Name5105A; "
    "class Pgm5105A "
    "  static method Take(a: IFoo5105A): int => a.Get(); "
    "  public static method Main(): int "
    "    var r := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      var p := new Name5105A(\"a\" + i.ToString(), \"b\"); "
    "      r := r + Take(p); "
    "      i := i + 1; "
    "    end while; "
    "    return r; "    // 200
    "  end Main; "
    "end Pgm5105A;";

// T2 — Same shape with Object parameter (not just user interface).
const char* kStringFieldRecordViaObject =
    "module M5105B; "
    "record Name5105B(A: string, B: string); "
    "class Pgm5105B "
    "  static method TakeObj(o: Object): int => 1; "
    "  public static method Main(): int "
    "    var r := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      var p := new Name5105B(\"a\" + i.ToString(), \"b\"); "
    "      r := r + TakeObj(p); "
    "      i := i + 1; "
    "    end while; "
    "    return r; "
    "  end Main; "
    "end Pgm5105B;";

// T3 — Control: int-field record through interface — #2952 already handles it.
const char* kIntFieldRecordViaInterface =
    "module M5105C; "
    "interface IFoo5105C "
    "  method Get(): int; "
    "end IFoo5105C; "
    "record Name5105C(A: int, B: int): IFoo5105C "
    "  public method Get(): int => 1; "
    "end Name5105C; "
    "class Pgm5105C "
    "  static method Take(a: IFoo5105C): int => a.Get(); "
    "  public static method Main(): int "
    "    var r := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      var p := new Name5105C(i, 0); "
    "      r := r + Take(p); "
    "      i := i + 1; "
    "    end while; "
    "    return r; "
    "  end Main; "
    "end Pgm5105C;";

}  // namespace

// =====================================================================
// T1 — THE PIN. String-field record through user interface parameter.
// LSan must report 0 leaks (the fix disposes the original record after boxing).
// =====================================================================
BEZEL_TEST(codegen_boxing_string_field_record_to_user_interface_leaks_5105,
           string_field_record_via_interface_no_leak_spec_8_6_28_2) {
    JitResult r = runAotChecked(kStringFieldRecordViaInterface);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LSan flips false
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// =====================================================================
// T2 — Object parameter twin. Same boxing-path leak per §8.6.
// =====================================================================
BEZEL_TEST(codegen_boxing_string_field_record_to_user_interface_leaks_5105,
           string_field_record_via_object_no_leak_spec_8_6) {
    JitResult r = runAotChecked(kStringFieldRecordViaObject);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LSan flips false
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// =====================================================================
// T3 — Control: int-field record through interface is already handled
// by the existing #2952 box-release mechanism.
// =====================================================================
BEZEL_TEST(codegen_boxing_string_field_record_to_user_interface_leaks_5105,
           int_field_record_via_interface_control_spec_8_6) {
    JitResult r = runAotChecked(kIntFieldRecordViaInterface);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}
