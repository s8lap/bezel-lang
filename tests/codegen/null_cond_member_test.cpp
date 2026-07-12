// Codegen slice CG41 — null-conditional member access `x?.M` (§7.5). If the
// target is null the result is null; otherwise the member is read. The
// result is always nullable, and `?.` short-circuits a chain. Both reference-
// typed members (null-pointer-as-null) and VALUE-typed members (`int`, `bool`,
// an enum, a record, and the `string.Length` intrinsic — lifted to the tagged
// {hasValue, value} `T?` struct, FINDING [3]) are covered. Observable via the
// int exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// N has a value field V and a self-referential nullable link `next`.
const char* kP =
    "module M; class N public V: int := 0; public next: N? := null; end N; "
    "class T public n: N? := null; end T; ";

} // namespace

BEZEL_TEST(codegen_cg41, member_present_yields_member) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var t := new T(); var inner := new N(); inner.V := 7; t.n := inner; "
        "  var r := t?.n; return r!.V; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg41, null_target_yields_null) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var t: T? := null; var r := t?.n; "
        "  if r = null then return 5; end; return 0; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg41, null_member_yields_null) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var t := new T(); var r := t?.n; "
        "  if r = null then return 9; end; return 0; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_cg41, chain_intact) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var a := new N(); var b := new N(); var c := new N(); c.V := 8; "
        "  a.next := b; b.next := c; var r := a?.next?.next; return r!.V; "
        "end Main; end C;"), 8);
}

BEZEL_TEST(codegen_cg41, chain_broken_in_middle_yields_null) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var a := new N(); var b := new N(); a.next := b; "
        "  var r := a?.next?.next; if r = null then return 4; end; return 0; "
        "end Main; end C;"), 4);
}

BEZEL_TEST(codegen_cg41, chain_first_link_null_short_circuits) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var a: N? := null; var r := a?.next?.next; "
        "  if r = null then return 6; end; return 0; "
        "end Main; end C;"), 6);
}

// FINDING [3] — a VALUE-typed member (`int` field) lifts to `int?`. A non-null
// receiver yields the field value wrapped in the tagged struct (§7.5: "the
// result type is always nullable").
BEZEL_TEST(codegen_cg41, value_int_field_non_null_yields_value) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 42; var t := new T(); t.n := n; "
        "  var inner := t?.n; var x: int? := inner?.V; return x!; "
        "end Main; end C;"), 42);
}

// FINDING [3] — a null receiver yields a null `int?` for a value-typed member
// (previously: non-null holding garbage).
BEZEL_TEST(codegen_cg41, value_int_field_null_receiver_yields_null) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var n: N? := null; var x: int? := n?.V; "
        "  if x = null then return 5; end; return 0; "
        "end Main; end C;"), 5);
}

// FINDING [3] — `int?` value member feeds `??` (the non-null receiver case
// supplies the field value; the null receiver case the default).
BEZEL_TEST(codegen_cg41, value_int_field_feeds_coalesce) {
    BEZEL_EXPECT_EQ(runProg(std::string(kP) +
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 7; var got: int := n?.V ?? 99; "
        "  var no: N? := null; var def: int := no?.V ?? 99; "
        "  return got + def; "  // 7 + 99
        "end Main; end C;"), 106);
}

// §7.5 example `user?.Name?.Length` — a value-typed `string.Length` member on a
// nullable string?. Non-null intermediate yields the actual length.
BEZEL_TEST(codegen_cg41, string_length_chain_non_null_intermediate) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class U public Name: string? := null; end U; "
        "class C public static method Main(): int "
        "  var u := new U(); u.Name := \"hello\"; "
        "  var length: int? := u?.Name?.Length; return length!; "
        "end Main; end C;"), 5);
}

// §7.5 example `user?.Name?.Length` — a null intermediate (`Name` is null)
// short-circuits to a null `int?`.
BEZEL_TEST(codegen_cg41, string_length_chain_null_intermediate_short_circuits) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class U public Name: string? := null; end U; "
        "class C public static method Main(): int "
        "  var u := new U(); "  // Name stays null
        "  var length: int? := u?.Name?.Length; "
        "  if length = null then return 6; end; return 0; "
        "end Main; end C;"), 6);
}

// §7.5 example `user?.Name?.Length` — a null OUTER receiver also short-circuits.
BEZEL_TEST(codegen_cg41, string_length_chain_null_receiver_short_circuits) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class U public Name: string? := null; end U; "
        "class C public static method Main(): int "
        "  var u: U? := null; "
        "  var length: int? := u?.Name?.Length; "
        "  if length = null then return 8; end; return 0; "
        "end Main; end C;"), 8);
}

// FINDING [3] — a `bool` value member lifts to `bool?` (non-null receiver).
BEZEL_TEST(codegen_cg41, value_bool_field_non_null) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class B public flag: bool := false; end B; "
        "class C public static method Main(): int "
        "  var b := new B(); b.flag := true; var bb: B? := b; "
        "  var x: bool? := bb?.flag; "
        "  if x = null then return 0; elif x! then return 1; else return 2; end; "
        "end Main; end C;"), 1);
}

// FINDING [3] — a member that is ITSELF nullable (`int?` field) keeps its own
// hasValue: a non-null receiver with a null field yields a null result.
BEZEL_TEST(codegen_cg41, already_nullable_field_passes_own_hasvalue) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class B public m: int? := null; end B; "
        "class C public static method Main(): int "
        "  var b := new B(); var bb: B? := b; "  // m is null, receiver non-null
        "  var x: int? := bb?.m; "
        "  if x = null then return 4; end; return 0; "
        "end Main; end C;"), 4);
}

// FINDING [3] — already-nullable `int?` field, non-null value, non-null receiver
// yields that value.
BEZEL_TEST(codegen_cg41, already_nullable_field_present_value) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class B public m: int? := 0; end B; "
        "class C public static method Main(): int "
        "  var b := new B(); b.m := 13; var bb: B? := b; "
        "  var x: int? := bb?.m; return x!; "
        "end Main; end C;"), 13);
}

// §7.5 / #751 — an INFERRED local from `?.` on a string built-in member
// (`var n := s?.Length`, no annotation) must carry the `int?` type so a
// following `??` yields the length, not 0. resolveNullCondMember previously
// left the result untyped (string built-in members have no Symbol and were not
// run through the intrinsic-typing block), so lowerCoalesce silently produced 0.
BEZEL_TEST(codegen_cg41, inferred_string_length_feeds_coalesce_751) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"ab\"; "
        "  var n := s?.Length; "   // inferred int? — was untyped pre-#751
        "  return n ?? 99; "       // 2, not 0
        "end Main; end C;"), 2);
}

// §7.5 / #751 — the null-receiver case still short-circuits: `s` is null, so
// `s?.Length` is a null `int?` and `?? 99` supplies the default.
BEZEL_TEST(codegen_cg41, inferred_string_length_null_receiver_coalesce_751) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var s: string? := null; "
        "  var n := s?.Length; "
        "  return n ?? 99; "       // 99
        "end Main; end C;"), 99);
}
