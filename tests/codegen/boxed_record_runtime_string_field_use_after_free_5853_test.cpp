// Codegen — #5853 / §8.6 "Boxing of borrow-bearing records" / §28.3 / §9.7
// (v0.334).
//
// A record boxed to an `Object`/interface formal must NOT release its string
// fields at the box site. #5105 (4b508ebf) added such a release to close a
// leak, and that release is a heap-use-after-free on any record whose string
// field was BUILT AT RUNTIME: `buildOrderedArgs` released the fields into a
// scratch slot BEFORE `boxForTarget` copied the record bytes, so the box was
// minted from a snapshot whose field pointers were already freed. Nothing
// re-balances them afterwards — `bz_box_release` treats the boxed value as a
// leaf (a value type, no owned sub-objects), so the record local ends up
// owning a freed pointer.
//
// WHAT ACTUALLY TRIGGERS IT (measured, and NOT what the issue's snippet does).
// The defect fires only when the call's FORMAL is declared `Object` or at an
// interface type — that is the gate on the box-site release
// (`isObjectOrInterfaceTarget(formalTy)` in `buildOrderedArgs`). The issue's
// repro passes the record to a formal declared at the RECORD'S OWN TYPE
// (`CheckN(a: Named, b: Named)`), so no box is minted at that call site at
// all: MEASURED, the issue's snippet compiles, runs and is ASan-CLEAN even
// with the #5105 release re-introduced (exit 2, zero sanitizer findings). It
// is a hypothesis about the shape, not a repro of it. Every row here therefore
// declares the formal at an interface.
//
// STRING LITERALS MASK IT. A literal carries the immortal sentinel refcount
// (i64 9223372036854775807), so releasing one is a no-op. T3 is the control
// that makes that asymmetry explicit rather than accidental: same program,
// same boxing path, literal field — clean before the fix and clean after it.
//
// EXPECTED EXIT = 2. The record's field is read back AFTER the boxing call
// and compared against the string it was built from. With the field live the
// comparison succeeds and Main returns 2. With the field freed, the dangling
// pointer compares as garbage, the comparison fails, and Main falls through
// to 1 — so the exit code is a second, sanitizer-independent discriminator.
//
// T1 is the PIN. T2 is the DIFFERENTIAL CONTROL against the obvious wrong fix
// ("stop freeing it", which trades a use-after-free for a leak): 200
// iterations of the same boxing path, LSan-clean. A one-shot leak repro is
// vacuous; 200 runtime-built strings make any per-record leak decisive. T2 is
// what distinguishes "freed exactly once" from "freed twice" and from "never
// freed".

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// T1 — THE PIN. A record with a RUNTIME-BUILT string field, passed to a formal
// declared at an interface (so the box path is actually taken), then read back
// off the same still-live local AFTER the call.
const char* kRuntimeBuiltStringFieldBoxed =
    "module M5853A; "
    "interface IFoo5853A "
    "  method Get(): string; "
    "end IFoo5853A; "
    "record Named5853A(Name: string, N: int): IFoo5853A "
    "  public method Get(): string => Name; "
    "end Named5853A; "
    "class Pgm5853A "
    "  static method Take(a: IFoo5853A): int => a.Get().Length; "
    "  public static method Main(): int "
    "    var s := \"x\"; "
    "    var t := s + \"y\"; "                 // RUNTIME-BUILT, not a literal
    "    var n1 := new Named5853A(t, 1); "
    "    var len := Take(n1); "               // BOXES n1
    "    if n1.Name = \"xy\" then return 2; end if; "  // reads the field back
    "    return 1; "
    "  end Main; "
    "end Pgm5853A;";

// T2 — THE DIFFERENTIAL CONTROL. Refutes "stop freeing it". 200 iterations of
// the boxing path with runtime-built string fields. runAotChecked links the
// generated program with ASan AND LeakSanitizer, so `r.ran` and the exit code
// are simultaneously the no-UAF and the no-growth oracle: pre-fix this is a
// UAF; with ONLY the box-site release removed (and no compensating owner-side
// release) it is the #5105 leak verbatim; with both halves it is clean.
//
// ITERATION COUNT IS 200, NOT 400, ON PURPOSE. The count is the program's exit
// code, and an exit code above 255 is truncated to 8 bits by the wait status —
// a 400-iteration run of this exact program measures clean under ASan/LSan but
// exits 144 (400 & 0xFF), which reads as a crash.
const char* kRuntimeBuiltStringFieldBoxedLoop =
    "module M5853C; "
    "interface IFoo5853C "
    "  method Get(): int; "
    "end IFoo5853C; "
    "record Name5853C(A: string, B: string): IFoo5853C "
    "  public method Get(): int => 1; "
    "end Name5853C; "
    "class Pgm5853C "
    "  static method Take(a: IFoo5853C): int => a.Get(); "
    "  public static method Main(): int "
    "    var r := 0; "
    "    var i := 0; "
    "    while i < 200 do "
    "      var p := new Name5853C(\"a\" + i.ToString(), \"b\"); "
    "      r := r + Take(p); "                // boxes p
    "      i := i + 1; "
    "    end while; "
    "    return r; "                           // 200
    "  end Main; "
    "end Pgm5853C;";

// T3 — the CONTROL SHAPE. The identical program with a COMPILE-TIME LITERAL
// field. Clean before the fix and clean after it. The asymmetry with T1 is the
// clue the issue names: literals carry the immortal sentinel refcount and mask
// a real ownership error.
const char* kLiteralStringFieldBoxed =
    "module M5853B; "
    "interface IFoo5853B "
    "  method Get(): string; "
    "end IFoo5853B; "
    "record Named5853B(Name: string, N: int): IFoo5853B "
    "  public method Get(): string => Name; "
    "end Named5853B; "
    "class Pgm5853B "
    "  static method Take(a: IFoo5853B): int => a.Get().Length; "
    "  public static method Main(): int "
    "    var lit := \"xy\"; "                  // COMPILE-TIME literal
    "    var n1 := new Named5853B(lit, 1); "
    "    var len := Take(n1); "
    "    if n1.Name = \"xy\" then return 2; end if; "
    "    return 1; "
    "  end Main; "
    "end Pgm5853B;";

}  // namespace

// =====================================================================
// T1 — THE PIN. Pre-fix: heap-use-after-free (READ of size 8) on the boxed
// record's freed string field; `r.ran` flips false and the exit code is 1,
// not 2, because the dangling field compares as garbage.
// =====================================================================
BEZEL_TEST(codegen_boxed_record_runtime_string_field_use_after_free_5853,
           boxed_record_runtime_string_read_after_call_spec_8_6_28_3) {
    JitResult r = runAotChecked(kRuntimeBuiltStringFieldBoxed);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: ASan flips this false
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// =====================================================================
// T2 — THE DIFFERENTIAL CONTROL. Refutes "stop freeing it" (which would trade
// the UAF for a leak). 200 runtime-built strings through the boxing path must
// be freed EXACTLY ONCE: no UAF, and no growth.
// =====================================================================
BEZEL_TEST(codegen_boxed_record_runtime_string_field_use_after_free_5853,
           boxed_record_runtime_string_loop_freed_exactly_once_spec_8_6_28_3) {
    JitResult r = runAotChecked(kRuntimeBuiltStringFieldBoxedLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // flips false on either a UAF or an LSan leak
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// =====================================================================
// T3 — the CONTROL SHAPE. The same program with a COMPILE-TIME literal field
// is clean. The asymmetry with T1 is the clue the issue names: literals carry
// the immortal sentinel refcount and mask a real ownership error.
// =====================================================================
BEZEL_TEST(codegen_boxed_record_runtime_string_field_use_after_free_5853,
           boxed_record_literal_string_field_control_spec_8_6) {
    JitResult r = runAotChecked(kLiteralStringFieldBoxed);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}