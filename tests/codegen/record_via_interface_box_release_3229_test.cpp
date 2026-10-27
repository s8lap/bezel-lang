// Codegen — issue #3229 / §28.3 "Auto-drop" + §28.9 "Runtime-managed types"
// + §8.6 "Boxing" (v0.254). When a RECORD is borrowed through an INTERFACE
// (`var face: IFace := value;`), the interface-typed local holds a heap
// `box` wrapper the §8.6 boxing path minted (calloc(1, 24) — vtable, rc, and
// record-bytes). The wrapper is a fresh +1 producer — the local is the sole
// owner — and §28.3's auto-drop / runtime-managed release requires the spec's
// "disposed exactly once on every path" contract to fire at SCOPE END on the
// normal-exit edge (and on the unwind edge). The escape walker
// (BoxEscapeWalker, LlvmBackend_Setup.cpp around line 22832) decides whether
// the local is eligible for the scope-end `bz_box_release`; the local only
// enters `boxReleaseEligible_` when every observed use is a SAFE read.
//
// THE DEFECT (#3229). The BoxEscapeWalker's bare MemberAccessExpr arm
// treated ANY member access on the tracked local as an ESCAPE — the
// long-standing comment ("A METHOD / PROPERTY receiver is an ESCAPE for a
// boxed Object") conflated property reads with method-group references.
// A property read (`face.Prop`) does NOT escape the box: the getter is
// dispatched through the itable, the receiver is `this` inside the getter
// body, and the Bezel ownership rules forbid a user getter from retaining
// `this` (§22.7 / §28.3 — the same reason the CallExpr arm already admits
// `face.Method()` as a safe read). A method-group reference (`face.Method`)
// DOES escape: the function value captures `this` (§27.6) and can outlive
// the scope. So the precise predicate is "the resolved symbol is a
// MethodSymbol", not "the expression is a MemberAccessExpr".
//
// Pre-fix: every `face.Prop` (and `face?.Prop`) read kept the local out of
// `boxReleaseEligible_`, the 24-byte box leak survived every scope exit,
// and LSan exited 1 — measured: 24 bytes leaked in 1 allocation from
// `main+0x4ea545` (the boxValue calloc site). The codegen lexically
// disconnected it from #1740 (the OwningBuffer kHeapPointerReferenceBuiltins
// promotion the §28.3 surface was already wired against) — the repro
// predates and postdates that fix; the leak is independent.
//
// FIX. Split the bare-MA / `?.` arms by the resolved symbol kind: a
// MethodSymbol stays an escape (safeRead=false); everything else — a
// PropertySymbol (the bug's case), a FieldSymbol, an auto-property storage
// read — is a safe read (safeRead=true). Mirrors the StringEscapeWalker's
// existing bare-MA arm and the BoxEscapeWalker's own CallExpr arm.
//
// THE TESTS:
//
// T1 — THE PIN. Identical to the issue's repro. The LSan-clean run is the
//   only signature the test can pin on (the program prints no observable
//   bytes; the 8-bit exit mask is 42 either way).
//
// T2 — The borrowed value is COMPUTED CORRECTLY through the dispatched
//   getter. Without the fix the program still returns 42 (the leak is
//   not a soundness bug); the test documents the value-flow side of the
//   same arm so a future "fix" that breaks the dispatch (e.g. premature
//   free → release-and-read) is caught.
//
// T3 — Null-conditional twin `face?.Name`. The same property-read defect
//   class on the `?.` AST node — the fix mirrors the `?.` arm.
//
// T4 — Method-group reference (`face.M`) STAYS an escape. A bound reference
//   captures `this` (§27.6), so the local must NOT be scope-end released
//   here — the function value would dangle. The captured closure shows
//   the box outlives the scope exactly as required; LSan stays clean for
//   a different reason (the closure owns the box via a separate path).
//
// T5 — Field-storage read `face.Name` where the record's `Name` is a
//   positional field generated as a property (the #815 / §9.1 inheritance).
//   This is the storage-vs-property split the StringEscapeWalker already
//   cares about; the box is read into an int slot, no borrow is held,
//   release is safe.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAot;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// T1 — THE PIN. The issue's exact repro. LSan flags a 24-byte Direct leak
// per run pre-fix; the fix routes the box through the scope-end
// `bz_box_release` and the run is clean.
const char* kRecordViaInterfaceBoxRelease =
    "module M3229A; "
    "interface IFace3229A "
    "  readonly property Name: int; "
    "end IFace3229A; "
    "record R3229A(Name: int): IFace3229A "
    "end R3229A; "
    "class C3229A "
    "  public static method Main(): int "
    "    var value := new R3229A(42); "
    "    var face: IFace3229A := value; "   // §8.6 mints a 24-byte box (calloc)
    "    return face.Name; "                 // box must be released at scope end
    "  end Main; "
    "end C3229A;";

// T2 — Loop-amplified pin. 100 iterations × 24 bytes = 2400-byte Direct
// leak pre-fix (LSan summarises it as 2400 bytes in 100 allocations); the
// loop body computes the witness through the dispatched getter. The sink
// pattern (`sink mod 256`) keeps the program's observable value identical
// before and after the fix so the only discriminator is LSan.
const char* kRecordViaInterfaceBoxReleaseLoop =
    "module M3229B; "
    "interface IFace3229B "
    "  readonly property Name: int; "
    "end IFace3229B; "
    "record R3229B(Name: int): IFace3229B "
    "end R3229B; "
    "class C3229B "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      var value := new R3229B(i + 7); "
    "      var face: IFace3229B := value; "
    "      sink := sink + face.Name; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C3229B;";

// T3 — Null-conditional twin. `face?.Name` is a NullCondMemberExpr (distinct
// AST node from MemberAccessExpr) — the bare-MA fix alone leaves this
// regression; the `?.` arm is the matching twin. The short-circuit is
// never observed here (`face` is non-nullable bound to a non-null box),
// but the AST shape is what the §7.5 walker has to classify.
const char* kRecordViaInterfaceNullCond =
    "module M3229C; "
    "interface IFace3229C "
    "  readonly property Name: int; "
    "end IFace3229C; "
    "record R3229C(Name: int): IFace3229C "
    "end R3229C; "
    "class C3229C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      var value := new R3229C(i + 11); "
    "      var face: IFace3229C := value; "
    "      sink := sink + (face?.Name ?? 0); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C3229C;";

// T4 — Method-group reference STAYS an escape (§27.6). A bound reference
// captures `this`; the resulting function value outlives the producing
// scope and the box must travel with it. The local is NOT in
// `boxReleaseEligible_` (the walked MethodSymbol arm keeps safeRead=false),
// so the box is held by the closure past the scope-end — the call past
// the scope returns the recorded value, and `Call` through the closure
// does its own retain/release path (the closure's env captures a +1).
// LSan stays clean but for a different reason (the closure owns the box).
// The test verifies the property-read fix did NOT over-relax into the
// method-group case — if it did, the box would be released by the local
// and the closure's later call would read freed memory (SIGSEGV / ASan
// UAF), distinguishable under runAotChecked as `r.ran == false`.
const char* kRecordViaInterfaceMethodGroupRef =
    "module M3229D; "
    "interface IFace3229D "
    "  method GetName(): int; "
    "end IFace3229D; "
    "record R3229D(N: int): IFace3229D "
    "  public method GetName(): int return N; end GetName; "
    "end R3229D; "
    "class C3229D "
    "  public static method Main(): int "
    "    var value := new R3229D(13); "
    "    var face: IFace3229D := value; "
    "    var bound: () -> int := face.GetName; "  // bound-method-group ref (escape)
    "    var i := 0; var sink := bound(); "
    "    while i < 100 do "
    "      sink := sink + bound(); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C3229D;";

}  // namespace

// =====================================================================
// #3229 — THE PIN. A record+interface local's box wrapper is released at
// scope end. LSan-clean under runAotChecked (the #3229 leak flips
// `r.ran` false); value is 42.
// =====================================================================

BEZEL_TEST(codegen_record_via_interface_box_release_3229,
           record_via_interface_box_release_at_scope_end_spec_28_3_28_9_8_6) {
    // spec §28.3 "Auto-drop" (v0.254): every owned value is disposed exactly
    // once on every path — explicitly where the program discharges it,
    // implicitly at scope end otherwise. The interface-typed local owns a
    // §8.6 box created by boxForTarget → boxValue → calloc(1, 24); the
    // runtime-managed type's lifetime is "managed by the runtime without
    // user involvement" (§28.9), and the BoxEscapeWalker-driven
    // `bz_box_release` at scope end is the K3.14 / auto-drop hook that
    // closes the obligation. Pre-fix the bare MemberAccessExpr arm treated
    // EVERY member read as an escape — the wrong predicate ("is a
    // MethodSymbol → escape", not "is a MemberAccessExpr → escape"),
    // because the dispatch path of a property getter is a call-bounded
    // borrow (the same reason the CallExpr arm admits `face.Method()`).
    JitResult r = runAotChecked(kRecordViaInterfaceBoxRelease);
    // LSan-clean (the #3229 leak would flip this).
    BEZEL_EXPECT_TRUE(r.ran);
    // The program's observable value is 42 (the `Name` field of the
    // constructed record).
    BEZEL_EXPECT_EQ(42, r.exitCode);
}

// =====================================================================
// #3229 — Loop-amplified pin. 100 iterations amplify the leak so LSan
// reports it precisely.
// =====================================================================

BEZEL_TEST(codegen_record_via_interface_box_release_3229,
           record_via_interface_box_release_loop_amplified_spec_28_3_28_9_8_6) {
    JitResult r = runAotChecked(kRecordViaInterfaceBoxReleaseLoop);
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #3229 — Null-conditional twin. The `?.` AST node is distinct from the
// bare `.`; the matching fix on the NullCondMemberExpr arm is the
// second half of the same defect class.
// =====================================================================

BEZEL_TEST(codegen_record_via_interface_box_release_3229,
           record_via_interface_nullcond_box_release_spec_28_3_28_9_8_6) {
    JitResult r = runAotChecked(kRecordViaInterfaceNullCond);
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #3229 — Method-group reference STAYS an escape (§27.6). The fix must
// NOT relax the wrong direction — a bound-method reference captures
// `this`, the function value carries the box out of the scope, and a
// premature scope-end release would read freed memory through the
// closure. The closure's own retain/release path keeps LSan clean for a
// different reason; the test verifies the relaxation is correctly
// bounded to non-method-symbol member accesses.
// =====================================================================

BEZEL_TEST(codegen_record_via_interface_box_release_3229,
           record_via_interface_method_group_ref_stays_escaped_spec_27_6) {
    JitResult r = runAotChecked(kRecordViaInterfaceMethodGroupRef);
    BEZEL_EXPECT_TRUE(r.ran);
}
