// Spec-derived regression tests for #2752: a `string` in an
// IEquatable<string> / IComparable<string> slot must dispatch as the §4.6
// runtime-managed REFERENCE it is, not as a boxed value.
//
// The conversion itself is correct and must stay accepted: stdlib §9.6 (v0.240)
// declares
//     public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>
// and stdlib §9's preamble says "`String` is the exception: it is a class, since
// `string` is a reference type with content equality (language spec §4.6)".
// Language spec §4.6 (v0.240): "An immutable sequence of `char` ... A reference
// type with content-based equality" and "`string` is **runtime-managed**".
// So REJECTING string -> IEquatable<string> would contradict the stdlib; the
// defect is on the consumer side.
//
// Before the fix, interface dispatch loaded `vptr = recv[0]` and then the itmap
// at `vptr[1]`. For a string, `recv` is a bare `bz_string*` whose word 0 is
// `char* data` (NOT a vtable — see bezel_runtime.h's bz_obj_is_string contract,
// §8.6/§12.7 #417), so the itmap load read 8 bytes at `data + 8`: a
// heap-buffer-overflow past the payload buffer, and a SIGSEGV in practice
// (plain, non-ASan builds of every shape below crashed with signal 11).
//
// The fix discriminates at run time with the same bz_obj_is_string helper the
// Object-typed receiver path (lowerObjectEquals) already uses, and routes a
// string receiver to the §9.6 content intrinsics. The controls below pin the
// other half: a receiver that is NOT a string must still reach its own itable
// slot, unchanged.
//
// Memory-safety fix, so the positive cases use runAotChecked (generated-code
// ASan + LSan); `.ran` is false on any ASan report or leak.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// ASan+LSan-checked run: -999 if the program did not run cleanly (crash, ASan
// report, or leak), otherwise Main()'s exit code.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
// Plain run, for the one shape that carries an unrelated pre-existing leak.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #2752 / §4.6 / stdlib §9.6: the minimal shape. A string in an
// IEquatable<string> slot dispatches Equals by CONTENT. Both terms are
// load-bearing: the second is non-zero only when Equals correctly REJECTS an
// unequal string, so a stub that always answers true (or always false) fails.
// Before the fix this crashed with SIGSEGV.
BEZEL_TEST(codegen_string_builtin_iface_2752, string_iequatable_equals_dispatches) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"alpha\"; "
        "  var e: IEquatable<string> := s; "
        "  return (if e.Equals(\"alpha\") then 10 else 0) "
        "       + (if e.Equals(\"beta\") then 0 else 1); "
        "end Main; end C;"), 11);
}

// #2752 / §4.6: content equality must hold for two DISTINCT string objects with
// equal content, not merely for the same pointer. This matters because the
// content-equality intrinsic takes a same-pointer fast path first, which two
// interned literals could satisfy without ever comparing content; the operand
// here is built at run time (interpolation) so it cannot be the same object as
// the literal it is compared against.
// Plain runAot, NOT runAotChecked: an interpolated temporary leaks 36 bytes in
// 2 allocations, and that leak is INDEPENDENT of this issue — the identical
// program with `var e: string := built;` and `=` in place of the interface slot
// and `.Equals` leaks exactly the same 36 bytes in 2 allocations. Filed
// separately; asserting the value here pins the dispatch without inheriting it.
BEZEL_TEST(codegen_string_builtin_iface_2752, string_iequatable_distinct_objects) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var n := 7; "
        "  var built := $\"v{n}\"; "
        "  var e: IEquatable<string> := built; "
        "  return (if e.Equals(\"v7\") then 20 else 0) "
        "       + (if e.Equals(\"v8\") then 0 else 2); "
        "end Main; end C;"), 22);
}

// #2752 / §4.6 / stdlib §9.6: IComparable<string>.CompareTo dispatches as
// ordinal lexicographic comparison ("`s < t` ... perform ordinal lexicographic
// comparison of the code-point sequence"). All three arms are load-bearing:
// less-than, greater-than, and equal. Before the fix this crashed with SIGSEGV.
BEZEL_TEST(codegen_string_builtin_iface_2752, string_icomparable_compareto_dispatches) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"mmm\"; "
        "  var c: IComparable<string> := s; "
        "  return (if c.CompareTo(\"nnn\") < 0 then 100 else 0) "
        "       + (if c.CompareTo(\"lll\") > 0 then 10 else 0) "
        "       + (if c.CompareTo(\"mmm\") = 0 then 1 else 0); "
        "end Main; end C;"), 111);
}

// #2752: the same defect through a METHOD PARAMETER typed IEquatable<string>
// (the conversion happens at the call boundary rather than a local
// initializer). Before the fix this crashed with SIGSEGV.
BEZEL_TEST(codegen_string_builtin_iface_2752, string_iequatable_through_parameter) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Take(e: IEquatable<string>, probe: string): bool "
        "    return e.Equals(probe); "
        "  end Take; "
        "  public static method Main(): int "
        "    var s := \"gamma\"; "
        "    return (if Take(s, \"gamma\") then 30 else 0) "
        "         + (if Take(s, \"delta\") then 0 else 3); "
        "  end Main; "
        "end C;"), 33);
}

// ── Controls: the NON-string half of the runtime discriminator must be
// completely unchanged. These are the false-routing hazard the fix could
// introduce — a class receiver wrongly sent to the string content intrinsic.

// #2752 control / stdlib §2.9: a user CLASS implementing IEquatable<string>
// must still reach ITS OWN itable slot. `Tag.Equals` deliberately IGNORES its
// argument and always answers true, so the assertion discriminates sharply: if
// the fix wrongly routed a class receiver into the string content compare, the
// probe "no-match-at-all" would answer FALSE and the result would be 0, not 40.
BEZEL_TEST(codegen_string_builtin_iface_2752, user_class_iequatable_string_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Tag: IEquatable<string> "
        "  private _n: string; "
        "  public constructor(n: string) _n := n; end constructor; "
        "  public method Equals(other: string): bool => true; "
        "end Tag; "
        "class C public static method Main(): int "
        "  using var t := new Tag(\"tag\"); "
        "  var e: IEquatable<string> := t; "
        "  return (if e.Equals(\"no-match-at-all\") then 40 else 0); "
        "end Main; end C;"), 40);
}

// #2752 control / stdlib §2.2: a user CLASS implementing IComparable<string>
// still reaches its own CompareTo.
// What makes this control sharp is runAotChecked, NOT the return value: 77 is
// inside the +/-255 band a byte-difference memcmp can legitimately produce, so
// the value alone would not prove correct routing. Under ASan it does — a
// wrongly-routed `Rank` receiver would have its vtable pointer read as
// `char* data` and its offset-8 word as a length, which fails the test on the
// memory error rather than on a value coincidence.
BEZEL_TEST(codegen_string_builtin_iface_2752, user_class_icomparable_string_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Rank: IComparable<string> "
        "  public constructor() end constructor; "
        "  public method CompareTo(other: string): int => 77; "
        "end Rank; "
        "class C public static method Main(): int "
        "  using var r := new Rank(); "
        "  var c: IComparable<string> := r; "
        "  return c.CompareTo(\"anything\"); "
        "end Main; end C;"), 77);
}

// #2752 control / #1311 / stdlib §9.1: a boxed PRIMITIVE in the same builtin
// interfaces is untouched — its box carries a real vtable at word 0, so the
// runtime discriminator sends it down the unchanged itable path.
BEZEL_TEST(codegen_string_builtin_iface_2752, boxed_int_iface_dispatch_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var e: IEquatable<int> := 7; "
        "  var c: IComparable<int> := 5; "
        "  return (if e.Equals(7) then 10 else 0) "
        "       + (if e.Equals(9) then 0 else 1) "
        "       + (if c.CompareTo(3) > 0 then 5 else 0); "
        "end Main; end C;"), 16);
}

// #2752 control / §9.6 (#946): a string reaching Equals through an OBJECT slot
// already worked (lowerObjectEquals' bz_obj_is_string branch) and must keep
// working — the fix reuses that helper and must not perturb this path.
BEZEL_TEST(codegen_string_builtin_iface_2752, string_through_object_slot_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"omega\"; "
        "  var o: Object := s; "
        "  return (if o.Equals(\"omega\") then 50 else 0) "
        "       + (if o.Equals(\"other\") then 0 else 5); "
        "end Main; end C;"), 55);
}

// ── Argument-side guard. emitStringEq / emitStringOrdCmp dereference BOTH
// operands as `bz_string` (loadStringLen reads offset 8, loadStringData offset
// 0), so the runtime discriminator must validate the ARGUMENT too, not only the
// receiver. That is reachable only through the IEquatable<Object> acceptance
// #2765 questions: there the argument is statically `Object` and need not be a
// string, whereas for IEquatable<string> it is statically a string.
//
// The governing sentence is §9.6's own declaration on `String`:
//     public pure override method Equals(other: Object?): bool;
//       // content-based, not reference-based
// so a string compared against a NON-string Object is `false` — this is the
// stdlib's answer, not an invention of the lowering.
//
// The next test is the true fail-without-fix WITNESS for the argument guard,
// and it is deterministic (address-independent, no ASan needed) — see its own
// comment. The two after it are broader shapes that pass either way; they are
// kept as ASAN TRIPWIRES rather than witnesses, and are labelled as such.

// #2752 / stdlib §9.6: the argument-side WITNESS. With a receiver-only guard
// this returns TRUE where §9.6 requires FALSE — a silent wrong answer with no
// crash and no ASan report.
//
// Why it is deterministic. A class instance is laid out [0] vtable pointer,
// [8] the §28.2 disposed flag (one byte, calloc-zeroed), then 8-byte-aligned
// storage fields at 16 (LlvmBackend_Layout.cpp). Misread as
// bz_string_struct { data@0, len@8 (i64), width@16 (i8) } a LIVE instance
// yields len = 0 (the zero flag plus zeroed padding) and width = the low byte
// of its first 8-aligned field. Give that field the value 1 and pair it with
// the receiver "" (Length 0, width 1): the length check passes (0 == 0), the
// width check passes (1 == 1), and the content compare degenerates to
// memcmp(_, _, 0), which returns 0 — i.e. EQUAL.
//
// ⚠ The field must be 8-byte-aligned for `width` to land on it: `_k: long`
// sits at offset 16, whereas a `bool` field would sit at 9 and the probe would
// read padding instead. Do not "simplify" the field type.
BEZEL_TEST(codegen_string_builtin_iface_2752, object_slot_equals_nonstring_is_not_true) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Key "
        "  private _k: long; "
        "  public constructor() _k := 1; end constructor; "
        "end Key; "
        "class C public static method Main(): int "
        "  using var k := new Key(); "
        "  var s := \"\"; "
        "  var v: Object := s; "
        "  var e: IEquatable<Object> := v; "
        "  var o: Object := k; "
        "  return (if e.Equals(o) then 0 else 80); "
        "end Main; end C;"), 80);
}

// ⚠ HONEST LABEL: the next two are NOT fail-without-fix witnesses — with the
// receiver-only guard they returned the SAME values, because for these operands
// the misread's length check fails and short-circuits to the correct `false`.
// They are kept as ASAN TRIPWIRES over two further heap shapes (a fieldless
// class, a boxed primitive): they run under the checked harness, so if the
// argument-side read is ever reintroduced and lands out of bounds they fail.
// The witness above is what pins the defect.
BEZEL_TEST(codegen_string_builtin_iface_2752, object_slot_equals_class_argument_is_false) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Thing public constructor() end constructor; end Thing; "
        "class C public static method Main(): int "
        "  using var t := new Thing(); "
        "  var s := \"payload-text\"; "
        "  var v: Object := s; "
        "  var e: IEquatable<Object> := v; "
        "  var o: Object := t; "
        "  return (if e.Equals(o) then 0 else 60); "
        "end Main; end C;"), 60);
}

// #2752: same argument-side guard with a BOXED PRIMITIVE argument — a different
// heap shape (box header + payload) from a class instance, so it exercises a
// different offset-0/offset-8 layout under the same discriminator.
// Plain runAot: boxing an Object leaks 24 bytes in 1 allocation here, which is
// unrelated to this issue (it is the box, not the dispatch).
BEZEL_TEST(codegen_string_builtin_iface_2752, object_slot_equals_boxed_argument_is_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"payload-text\"; "
        "  var v: Object := s; "
        "  var e: IEquatable<Object> := v; "
        "  var b: Object := true; "
        "  return (if e.Equals(b) then 0 else 61); "
        "end Main; end C;"), 61);
}

// #2752 control: the argument-side guard must not disturb the shape this issue
// is actually about. For IEquatable<string> the argument is statically a
// string, so the added test always passes and both operands take the content
// path — including a string ARGUMENT that is a distinct runtime-built object.
BEZEL_TEST(codegen_string_builtin_iface_2752, string_slot_unaffected_by_argument_guard) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var n := 4; "
        "  var a := $\"w{n}\"; "
        "  var b := $\"w{n}\"; "
        "  var e: IEquatable<string> := a; "
        "  return (if e.Equals(b) then 70 else 0) "
        "       + (if e.Equals(\"w5\") then 0 else 7); "
        "end Main; end C;"), 77);
}

// #2752: the issue's own reported repro — a string temporary flowing out of an
// indexer whose declared result type is IEquatable<string>, in a loop with
// allocation churn between the store and the read (which is what made the
// out-of-bounds read land on an unmapped page). Uses plain runAot, NOT
// runAotChecked: this shape also leaks, but that leak is INDEPENDENT of this
// issue — the identical program with the property typed `string` instead of
// `IEquatable<string>` leaks the same 751 bytes in 40 allocations, i.e. it is
// the indexer-temporary defect (#2717 family), not this one. Asserting the
// value here pins the memory-safety fix without inheriting that leak.
// 20 iterations x (1 for the loop body + 1 for the matching Equals) = 40.
BEZEL_TEST(codegen_string_builtin_iface_2752, issue_repro_indexer_temporary) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class EchoTwo "
        "  public property [kk: string]: IEquatable<string> "
        "    get => kk; "
        "  end; "
        "end EchoTwo; "
        "class C public static method Main(): int "
        "  using var e := new EchoTwo(); "
        "  var acc := 0; "
        "  for var j in 1..20 do "
        "    var got: IEquatable<string> := e[$\"yy-{j}\"]; "
        "    var noise := $\"noise-noise-noise-{j}-{j}-{j}\"; "
        "    acc := acc + noise.Length - noise.Length + 1; "
        "    if got.Equals($\"yy-{j}\") then acc := acc + 1; end; "
        "  end; "
        "  return acc; "
        "end Main; end C;"), 40);
}
