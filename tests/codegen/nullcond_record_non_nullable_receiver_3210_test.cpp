// Codegen — issue #3210. `p?.M` where p is a NON-NULLABLE record value
// SILENTLY DROPPED the whole enclosing statement — no diagnostic at any
// severity, exit 0. The `?.` member-access lowering for a record receiver
// had a nullable-record arm that lowered the tagged {hasValue, payload}
// struct, but no parallel arm for a non-nullable receiver (whose lowering
// is a bare by-value struct). The "always" arm at FINDING [3] expected a
// pointer receiver and produced nullptr on a by-value struct, so
// Console.WriteLine's `internal:` note fired on stderr while nothing
// reached stdout — exit 0 with the line MISSING entirely (the standard
// #3079 silent-drop mechanism; not even the line terminator).
//
// Spec: §7.5 "Null-conditional member access":
//
//   `x?.M` returns null if `x` is null; otherwise it performs the access.
//   The result type is always nullable.
//
// For a NON-nullable receiver (§7.1 / §7.5, v0.254), the receiver cannot be
// null, so the result is the lifted member access (the same lift a present
// nullable-record receiver produces). The bug was accepting the access but
// emitting nothing. §9.1 (positional records) defines the by-value lowering;
// §9.2 §9.8 (inherited records) defines the field-address path; v0.254.
//
// THE DEFECT. CG41 / §7.5 in `Emitter::lowerExprInner` for
// `NullCondMemberExpr` had a record arm keyed on `isNullableRecordTy(tt)`:
// the receiver's LLVM IR shape was the nullable-tagged {i8 hasValue; payload}
// struct. A NON-nullable record receiver lower to the BARE by-value payload
// struct (§9.4), so it failed the gate, fell to FINDING [3] ("receiver is a
// class/string reference, so a null receiver is the null pointer"), failed
// the `isPointerTy` guard there (a by-value record is a struct, not a
// pointer), and returned nullptr — exactly the `storageMemberOf`-null silent
// drop the §31.4 / #2990 / #3015 / #3016 / #3209 family tracks for the other
// receiver shapes.
//
// MEASURED on ddf41f2: the issue's exact repro
//
//   var p1 := new PtP3L(11, 22);          // non-nullable record
//   Console.WriteLine("a=" + (p1?.X ?? 99).ToString());
//   var p2: PtP3L? := new PtP3L(11, 22);  // nullable record (worked)
//   Console.WriteLine("b=" + (p2?.X ?? 99).ToString());
//   var p3: PtP3L? := null;              // nullable record (worked)
//   Console.WriteLine("c=" + (p3?.X ?? 99).ToString());
//
// → compiled clean, exited 0, internal:` note on stderr for line 8, stdout
// was "b=11\nc=99\n" — the `a=` line was MISSING ENTIRELY. Identical
// single-module; not an imported-symbol defect; not affected by any prior
// cycle's fix.
//
// FIX. Extend the §7.5 record arm from the nullable-record-only gate to
// "any record receiver" (named TypeRef whose declaration is a record,
// nullable OR not), branch on `tt->nullable` to select the runtime shape
// — nullable: tagged {hasValue, payload} → run nullablePrimHasValue /
// nullablePrimValue; non-nullable: bare payload → hasValue is constant
// i1 1. The rest of the arm (field extract, generic-subst setup, result
// lift to §7.5 nullable) is the existing path's logic, unmodified.
//
// New helpers (none): the fix uses `tt->kind == Named`, the existing
// `tt->declaration->kind == TypeDeclKind::Record` discriminant
// (`isNullableRecordTy` is one branch of it, retained for the §7.2 wording
// the docstring traces), `nullablePrimHasValue` / `nullablePrimValue`
// (nullable path), `recordMemberTypeResolved` / `recordStruct` /
// `fieldIndex_` (field extract — same as before).
//
// ── THE TESTS ────────────────────────────────────────────────────────────
//
// Every T asserts the program's OUTPUT BYTES. "Compiled" and "exit 0"
// are not evidence for a silent-drop bug: T1 compiles clean and exits 0
// both before and after the fix — the only discriminator is the substring
// "a=11" appearing in stdout. Pre-fix: "b=11\nc=99\n"; post-fix:
// "a=11\nb=11\nc=99\n". The 8-bit exit mask (`rc % 256` ≡ `WEXITSTATUS`)
// cannot distinguish "ran and did nothing" from "did not run" so the line's
// PRESENCE is the only signature the test can pin on.
//
// T1 — THE PIN. Identical to the issue's repro. Fails pre-fix (no "a=…").
// T2 — The existing nullable-receiver path STAYS correct (T2 is the
//   differential control: the nullable branch is the same code it was).
// T3 — A null nullable-record receiver still short-circuits to null and
//   the consumer's `??` picks the default (sanity that T2/T3 didn't
//   regress).
// T4 — A NON-NULLABLE record with a REFERENCE-TYPED field (string?).
//   Records do not normally hold reference fields under §9.2, but the
//   class-typed field case is the natural analogue and exercises the
//   field-as-pointer lift branch of the new code (line 2020 of the
//   pre-fix file).
// T5 — A NON-NULLABLE record with a RECORD-VALUED field (nested record).
//   The field is itself a value, so the result lift wraps it into its
//   own {hasValue, payload} form per §7.5 — a second invocation of the
//   fixed arm, exercising the value-typed-field branch (line 2040).
// T6 — A NON-NULLABLE record whose VALUE-TYPED field is itself nullable
//   (`int?`). The nullable-field branch at line 2027 collapses to a
//   pass-through of the receiver's constant-1 hasValue for the non-
//   nullable receiver.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// The canonical record from the issue.
const char* kPt3210 = "record Pt3210(X: int, Y: int); ";

// A record with a reference-typed field (string) for the pointer-arm lift.
const char* kNamed3210 = "record Named3210(Name: string, Age: int); ";

// A NESTED record: outer has a field whose type is itself a record.
const char* kInner = "record Inner3210(Px: int); ";
const char* kOuter = "record Outer3210(Ix: int, Iy: Inner3210); ";

// A record whose VALUE-TYPED field is itself nullable (a `int?`).
const char* kMaybeX = "record MaybeX3210(Q: int, S: int?); ";

}  // namespace

// ===========================================================================
// T1 — THE PIN. §7.5: `p1?.X ?? 99` on a NON-NULLABLE record `p1`. Pre-fix:
// the WriteLine emits nothing (no `a=` line at all, exit 0); post-fix:
// `a=11`. // per §7.5 / §9.1, v0.254.
// ===========================================================================
BEZEL_TEST(codegen_nullcond_record_non_nullable_receiver_3210,
           non_nullable_record_qmark_field_emits_value_spec_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M3210a; ") + kPt3210 +
        "public class C "
        "  public static method Main(): int "
        "    var p1 := new Pt3210(11, 22); "
        "    Console.WriteLine(\"a=\" + (p1?.X ?? 99).ToString()); "
        "    var p2: Pt3210? := new Pt3210(11, 22); "
        "    Console.WriteLine(\"b=\" + (p2?.X ?? 99).ToString()); "
        "    var p3: Pt3210? := null; "
        "    Console.WriteLine(\"c=\" + (p3?.X ?? 99).ToString()); "
        "    return 0; "
        "end Main; "
        "end C;"),
        std::string("a=11\nb=11\nc=99"));
}

// ===========================================================================
// T2 — NULLABLE record control. The pre-existing path (#416, §7.5/§7.2) must
// stay correct: same shape, but with a nullable-receiver local. // per §7.5 /
// §7.2 / §9.4, v0.254.
// ===========================================================================
BEZEL_TEST(codegen_nullcond_record_non_nullable_receiver_3210,
           nullable_record_qmark_field_still_emits_spec_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M3210b; ") + kPt3210 +
        "public class C "
        "  public static method Main(): int "
        "    var p2: Pt3210? := new Pt3210(11, 22); "
        "    Console.WriteLine(\"b=\" + (p2?.X ?? 99).ToString()); "
        "    return 0; "
        "end Main; "
        "end C;"),
        std::string("b=11"));
}

// ===========================================================================
// T3 — NULL nullable-record receiver still short-circuits. // per §7.5, v0.254.
// ===========================================================================
BEZEL_TEST(codegen_nullcond_record_non_nullable_receiver_3210,
           nullable_record_null_receiver_short_circuits_spec_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M3210c; ") + kPt3210 +
        "public class C "
        "  public static method Main(): int "
        "    var p3: Pt3210? := null; "
        "    Console.WriteLine(\"c=\" + (p3?.X ?? 99).ToString()); "
        "    return 0; "
        "end Main; "
        "end C;"),
        std::string("c=99"));
}

// ===========================================================================
// T4 — NON-NULLABLE record with a REFERENCE-TYPED field. The `Name: string`
// field lower to a pointer; the §7.5 result is a (possibly-null) pointer,
// null when the receiver is null (a constant false for a non-nullable
// receiver per the fix's `hasVal = 1`). // per §7.5 / §9.4, v0.254.
// ===========================================================================
BEZEL_TEST(codegen_nullcond_record_non_nullable_receiver_3210,
           non_nullable_record_qmark_ref_field_preserved_spec_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M3210d; ") + kNamed3210 +
        "public class C "
        "  public static method Main(): int "
        "    var n := new Named3210(\"alice\", 30); "
        "    Console.WriteLine(\"n=\" + (n?.Name ?? \"<null>\")); "
        "    return 0; "
        "end Main; "
        "end C;"),
        std::string("n=alice"));
}

// ===========================================================================
// T5 — NON-NULLABLE record with a NESTED record field. The `Iy: Inner3210`
// field is a by-value record; the §7.5 lift wraps it into the tagged
// {hasValue, payload} nullable form per the value-typed-field branch. //
// per §7.5 / §9.4, v0.254.
// ===========================================================================
BEZEL_TEST(codegen_nullcond_record_non_nullable_receiver_3210,
           non_nullable_record_qmark_nested_record_field_spec_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M3210e; ") + kInner + kOuter +
        "public class C "
        "  public static method Main(): int "
        "    var o := new Outer3210(7, new Inner3210(13)); "
        "    Console.WriteLine(\"i=\" + (o?.Iy?.Px ?? 99).ToString()); "
        "    return 0; "
        "end Main; "
        "end C;"),
        std::string("i=13"));
}

// ===========================================================================
// T6 — NON-NULLABLE record whose VALUE-TYPED field is itself nullable
// (`S: int?`). The §7.5 nullable-field branch at the pre-fix line 2027
// collapses to a pass-through of the receiver's constant-1 hasValue
// (the AND with `hasVal = 1` is `innerHas`). Pre-fix would still drop —
// the receiver fell to FINDING [3] before any lift could run. // per §7.5,
// v0.254.
// ===========================================================================
BEZEL_TEST(codegen_nullcond_record_non_nullable_receiver_3210,
           non_nullable_record_qmark_nullable_val_field_passthrough_spec_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M3210f; ") + kMaybeX +
        "public class C "
        "  public static method Main(): int "
        "    var m := new MaybeX3210(5, 17); "
        "    Console.WriteLine(\"s=\" + (m?.S ?? 99).ToString()); "
        "    return 0; "
        "end Main; "
        "end C;"),
        std::string("s=17"));
}
