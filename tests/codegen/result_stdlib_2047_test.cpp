// Codegen behavior tests for the stdlib `Result<T, E>` choice — stdlib §2.20
// (v0.200), language spec §35 (choices), §28.1 (per-instantiation `owned`
// erasure), §35.4 (classification + disposal), §35.7 (equality/hash). Issue
// #2047.
//
// `Result<T, E>` is the ONE choice the stdlib ships (§2.20), AUTO-IMPORTED: user
// code names `Result<T, E>` with no declaration. It is synthesized and injected
// as a top-level choice into every compilation unit (Driver::injectStdlibPrelude),
// so the full choice machinery applies to it exactly as to a user-declared
// choice. The documented surface is exactly:
//
//   public choice Result<T, E>
//     Ok(owned value: T),
//     Error(owned error: E);
//     public method IsOk(): bool => (case this of Ok(_) => true, Error(_) => false);
//     public method IsError(): bool => not IsOk();
//   end Result;
//
// These tests pin, end-to-end (AOT compile + native run):
//   * §2.20 auto-import + construction of BOTH cases + exhaustive match.
//   * §28.1 per-instantiation erasure: `Result<int, string>` is value-like
//     (both `owned` markers erase over int / string), so an ordinary value /
//     borrow argument is accepted (the stdlib `ParsePort` pattern), it is
//     equality-bearing, and usable as a Dictionary key.
//   * §35.4 owning instantiation: `Result<C, string>` disposes the active
//     case's owned CLASS payload exactly once and never the `string` payload
//     (which erased), leak- and double-free-clean (runAotChecked / ASan).
//   * §35.7 equality over value and string payloads.
//   * IsOk / IsError (the two documented members), over `this`.
//   * generic propagation of `Result<T, E>` through a `where T: hashable`
//     generic (the value-like instantiation satisfies the constraint).
//
// Serialization is deliberately NOT tested: `Result` declares NO interface
// conformances (§2.20) — in particular not ISerializable/IDeserializable — so it
// is not serialization-eligible, correctly.
//
// APIs verified against Bezel_Standard_Library.md: `Result<T, E>` (§2.20),
// `Dictionary<K, V>` (§4.8), `string.Length` (§4.6).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Wrap a Main body in the standard module + host-class shell and run it; return
// the exit code (-999 if the compile/run failed). The host class is `Host` so a
// test's own helper `class C` / `class Conn` (passed in `decls`) never collides.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class Host public static method Main(): int ") +
        body + " end Main; end Host;");
    return r.ran ? r.exitCode : -999;
}

// As `run`, with extra top-level declarations (helper classes) before the host.
int runDecls(const std::string& decls, const std::string& body) {
    auto r = runAot(
        std::string("module M; ") + decls +
        std::string(" class Host public static method Main(): int ") +
        body + " end Main; end Host;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// --- §2.20 auto-import + construction + exhaustive match -------------------

// §2.20 / §35.3 / §35.5: `Result<int, string>` is AVAILABLE with no declaration;
// the `Ok` case constructs and the matching `Ok(var v)` arm binds+reads the
// payload. §35.5: the two-case match is exhaustive with NO `_` wildcard.
BEZEL_TEST(codegen_result_2047, spec_2_20_auto_import_ok_construct_match) {
    BEZEL_EXPECT_EQ(run(
        "var r: Result<int, string> := Result<int, string>.Ok(5); "
        "var acc: int := 0; "
        "case r of "
        "  Ok(var v)    => do acc := v; end, "
        "  Error(var m) => do acc := 0; end "
        "end; "
        "return acc;"), 5);
}

// §2.20 / §28.1: the `Error` case carries a `string` payload; at `Result<int,
// string>` the `owned error: T` marker ERASES over `string` (runtime-managed),
// so the choice is value-like and the string payload round-trips (its content
// is readable — Length of "hello" is 5). Pins the generic-choice payload-layout
// specialization: the Error slot lowers to a string pointer, not the i32
// placeholder.
BEZEL_TEST(codegen_result_2047, spec_2_20_error_string_payload_roundtrip) {
    BEZEL_EXPECT_EQ(run(
        "var r: Result<int, string> := Result<int, string>.Error(\"hello\"); "
        "var acc: int := 0; "
        "case r of "
        "  Ok(var v)    => do acc := v; end, "
        "  Error(var m) => do acc := m.Length; end "
        "end; "
        "return acc;"), 5);
}

// --- §2.20 documented members: IsOk / IsError ------------------------------

// §2.20: `IsOk()` (`=> (case this of Ok(_) => true, Error(_) => false)`) — a
// choice instance method whose body matches over `this`. §35.2: `this` is a
// borrow of the choice.
BEZEL_TEST(codegen_result_2047, spec_2_20_isok_member_true_on_ok) {
    BEZEL_EXPECT_EQ(run(
        "var r: Result<int, string> := Result<int, string>.Ok(9); "
        "if r.IsOk() then return 1; end; "
        "return 0;"), 1);
}

// §2.20: `IsError()` (`=> not IsOk()`) — an instance method that CALLS another
// instance method through implicit `this`.
BEZEL_TEST(codegen_result_2047, spec_2_20_iserror_member_true_on_error) {
    BEZEL_EXPECT_EQ(run(
        "var r: Result<int, string> := Result<int, string>.Error(\"boom\"); "
        "if r.IsError() then return 1; end; "
        "return 0;"), 1);
}

// §2.20: IsOk is false / IsError is true consistently on the Error case (and the
// reverse on Ok) — the two members are complementary (`IsError := not IsOk`).
BEZEL_TEST(codegen_result_2047, spec_2_20_isok_iserror_complementary) {
    BEZEL_EXPECT_EQ(run(
        "var a: Result<int, string> := Result<int, string>.Ok(1); "
        "var b: Result<int, string> := Result<int, string>.Error(\"e\"); "
        "var acc: int := 0; "
        "if a.IsOk() then acc := acc + 1; end; "
        "if not a.IsError() then acc := acc + 2; end; "
        "if b.IsError() then acc := acc + 4; end; "
        "if not b.IsOk() then acc := acc + 8; end; "
        "return acc;"), 15);
}

// --- §28.1 per-instantiation erasure: value-like `owned` payload -----------

// §28.1 / §2.20: the stdlib `ParsePort` pattern — `Result<int, string>.Ok(value)`
// where `value` is an ordinary local `int`. The `owned value: T` marker ERASES
// over T=int, so a value/borrow argument is accepted (no ownership transfer
// required). Without the erasure this rejects with a §28.8 "borrow into owned
// sink" error.
BEZEL_TEST(codegen_result_2047, spec_28_1_owned_payload_marker_erased_value_arg) {
    BEZEL_EXPECT_EQ(run(
        "var value: int := 7; "
        "var r: Result<int, string> := Result<int, string>.Ok(value); "
        "var acc: int := 0; "
        "case r of "
        "  Ok(var v)    => do acc := v; end, "
        "  Error(var m) => do acc := 0; end "
        "end; "
        "return acc;"), 7);
}

// §28.1: the same erasure holds when the value argument is a METHOD PARAMETER
// returned directly (`return Result<int, string>.Ok(v)`) — the return-context
// owned-sink check must also apply the erasure.
BEZEL_TEST(codegen_result_2047, spec_28_1_owned_payload_erased_param_return) {
    BEZEL_EXPECT_EQ(runDecls(
        "class Lib "
        "  public static method Wrap(v: int): Result<int, string> "
        "    return Result<int, string>.Ok(v); "
        "  end Wrap; "
        "end Lib;",
        "var r: Result<int, string> := Lib.Wrap(6); "
        "var acc: int := 0; "
        "case r of "
        "  Ok(var v)    => do acc := v; end, "
        "  Error(var m) => do acc := 0; end "
        "end; "
        "return acc;"), 6);
}

// --- §35.7 equality (value-like auto-equality-bearing) ---------------------

// §35.7 / §28.1: `Result<int, string>` is value-like, hence AUTO-EQUALITY-
// BEARING — `=` / `<>` are available. Equal Ok payloads compare equal; a
// different active case is unequal.
BEZEL_TEST(codegen_result_2047, spec_35_7_equality_int_payload) {
    BEZEL_EXPECT_EQ(run(
        "var a: Result<int, string> := Result<int, string>.Ok(5); "
        "var b: Result<int, string> := Result<int, string>.Ok(5); "
        "var c: Result<int, string> := Result<int, string>.Error(\"x\"); "
        "var acc: int := 0; "
        "if a = b then acc := acc + 1; end; "
        "if a <> c then acc := acc + 2; end; "
        "return acc;"), 3);
}

// §35.7: equality of the STRING payload compares by CONTENT (not pointer
// identity) — two `Error("boom")` are equal; `Error("boom")` and `Error("nope")`
// are not. Pins the per-payload equality resolving the concrete `string` type at
// the instantiation.
BEZEL_TEST(codegen_result_2047, spec_35_7_equality_string_payload_by_content) {
    BEZEL_EXPECT_EQ(run(
        "var a: Result<int, string> := Result<int, string>.Error(\"boom\"); "
        "var b: Result<int, string> := Result<int, string>.Error(\"boom\"); "
        "var c: Result<int, string> := Result<int, string>.Error(\"nope\"); "
        "var acc: int := 0; "
        "if a = b then acc := acc + 1; end; "
        "if a <> c then acc := acc + 2; end; "
        "return acc;"), 3);
}

// --- §2.20 / §3.6: Result as a Dictionary key ------------------------------

// §2.20 / §3.6 / §35.7: a value-like `Result<int, string>` is hashable-
// admissible, so it is a valid `Dictionary<K, V>` key — keys hash+compare by
// tag + active payload (a string payload by content). Two distinct-case keys
// coexist and look up correctly.
BEZEL_TEST(codegen_result_2047, spec_2_20_dictionary_key) {
    BEZEL_EXPECT_EQ(run(
        "var d: Dictionary<Result<int, string>, int> := "
        "  new Dictionary<Result<int, string>, int>(); "
        "d[Result<int, string>.Ok(5)] := 100; "
        "d[Result<int, string>.Error(\"boom\")] := 7; "
        "var acc: int := 0; "
        "if d.ContainsKey(Result<int, string>.Ok(5)) then "
        "  acc := acc + d[Result<int, string>.Ok(5)]; end; "
        "if d.ContainsKey(Result<int, string>.Error(\"boom\")) then "
        "  acc := acc + d[Result<int, string>.Error(\"boom\")]; end; "
        "return acc;"), 107);
}

// --- §14.2 generic propagation --------------------------------------------

// §14.2 / §35.7: `Result<int, string>` flows through a `where T: hashable`
// generic container (the value-like instantiation satisfies `hashable`),
// constructed from a method parameter, stored, retrieved, and matched.
BEZEL_TEST(codegen_result_2047, spec_14_2_generic_propagation_hashable) {
    BEZEL_EXPECT_EQ(runDecls(
        "class Box<T> where T: hashable "
        "  public slot: T; "
        "  public constructor(v: T) slot := v; end constructor; "
        "  public method Get(): T => slot; "
        "end Box;",
        "var b: Box<Result<int, string>> := "
        "  new Box<Result<int, string>>(Result<int, string>.Ok(5)); "
        "var r: Result<int, string> := b.Get(); "
        "var acc: int := 0; "
        "case r of "
        "  Ok(var v)    => do acc := v; end, "
        "  Error(var m) => do acc := 0; end "
        "end; "
        "return acc;"), 5);
}

// --- §35.4 owning instantiation: disposal ----------------------------------

// §35.4 / §28.1: `Result<C, string>` is OWNING (the `Ok(owned value: T)` marker
// is ACTIVE over a class payload). At scope end the active case's owned CLASS
// payload is disposed EXACTLY ONCE; the `Error(owned error: string)` payload's
// marker ERASED (string is runtime-managed) so it is NEVER vtable-disposed. A
// destructor counter observes the Ok(C) disposal (+1) and the Error("x") non-
// disposal (+0). runAotChecked runs under ASan/LSan: a leak or double-free (e.g.
// disposing the string as a class) would flip `r.ran` to false.
BEZEL_TEST(codegen_result_2047, spec_35_4_owning_instantiation_disposal) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end C; "
        "class Host public static method Main(): int "
        "  do using var ok: Result<C, string> := "
        "       Result<C, string>.Ok(new C()); end; "
        "  do using var err: Result<C, string> := "
        "       Result<C, string>.Error(\"x\"); end; "
        "  return C._n; "
        "end Main; end Host;");
    BEZEL_EXPECT_TRUE(r.ran);          // ASan/LSan-clean: no leak, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 1);    // Ok disposes C once; Error string not disposed
}

// §35.4: `IsOk` / `IsError` work on the OWNING instantiation too (a borrow
// match over `this` — no consumption), alongside the owned-payload disposal.
BEZEL_TEST(codegen_result_2047, spec_35_4_owning_isok_iserror) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end C; "
        "class Host public static method Main(): int "
        "  var acc: int := 0; "
        "  do using var ok: Result<C, string> := "
        "       Result<C, string>.Ok(new C()); "
        "     if ok.IsOk() then acc := acc + 1; end; end; "
        "  do using var err: Result<C, string> := "
        "       Result<C, string>.Error(\"x\"); "
        "     if err.IsError() then acc := acc + 2; end; end; "
        "  return acc; "
        "end Main; end Host;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// --- §35.5 / §19.3: case-name vs. class-name collision ---------------------

// §35.5 / §19.3 (#2047 follow-up): a user `class Ok` shares the NAME of the
// injected `Result`'s `Ok` case. The prelude's `IsOk` (`case this of Ok(_) …`)
// must resolve `Ok` to the CASE, not the user class — per §35.5 "a case name
// resolves to a case pattern, never a type test". Before the fix this reported a
// spurious "positional destructuring pattern `Ok(...)` is not valid on a class
// type (§19.3)" inside the prelude and failed the whole compile; now Result and
// the user class coexist and Result runs. (This is the general defect — `this`
// is untyped for a named-type method, so the scrutinee-choice-first resolution
// was skipped.)
BEZEL_TEST(codegen_result_2047, spec_35_5_user_class_named_after_case_ok_coexists) {
    BEZEL_EXPECT_EQ(runDecls(
        "class Ok "
        "  public v: int; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Ok;",
        "var r: Result<int, string> := Result<int, string>.Ok(5); "
        "var acc: int := 0; "
        "if r.IsOk() then acc := acc + 5; end; "        // prelude IsOk resolves
        "case r of "                                     // its own `Ok` case
        "  Ok(var v)    => do acc := acc + v; end, "     // user's `case r of Ok`
        "  Error(var m) => do acc := 0; end "            // also resolves the case
        "end; "
        "return acc;"), 10);
}

// §35.5 / §19.3: the GENERAL shape — a USER choice `Opt` with an instance method
// `case this of Some(_) …`, where `Some` collides with a user `class Some`. The
// pattern head resolves to the choice's case, so `HasValue()` works. Pins that
// the fix is general (any choice instance method over `this`), not prelude-only.
BEZEL_TEST(codegen_result_2047, spec_35_5_user_choice_method_case_name_class_collision) {
    BEZEL_EXPECT_EQ(runDecls(
        "class Some "
        "  public v: int; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Some; "
        "choice Opt "
        "  Some(value: int), "
        "  None; "
        "  public method HasValue(): bool => (case this of "
        "    Some(_) => true, "
        "    None    => false); "
        "end Opt;",
        "var o: Opt := Opt.Some(9); "
        "if o.HasValue() then return 7; end; "
        "return 0;"), 7);
}

// §35.5 CONTROL: with NO colliding class, a choice instance method's `case this`
// still resolves correctly — the recovery does not perturb the normal path.
BEZEL_TEST(codegen_result_2047, spec_35_5_no_collision_case_this_still_resolves) {
    BEZEL_EXPECT_EQ(runDecls(
        "choice Opt "
        "  Some(value: int), "
        "  None; "
        "  public method HasValue(): bool => (case this of "
        "    Some(_) => true, "
        "    None    => false); "
        "end Opt;",
        "var a: Opt := Opt.Some(4); "
        "var b: Opt := Opt.None; "
        "var acc: int := 0; "
        "if a.HasValue() then acc := acc + 1; end; "
        "if not b.HasValue() then acc := acc + 2; end; "
        "return acc;"), 3);
}
