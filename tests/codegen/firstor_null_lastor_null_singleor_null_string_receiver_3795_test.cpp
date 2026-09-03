// Codegen + Sema — stdlib §5 / §9.6 (v0.286), issue 3795: the
// `*OrNull` element terminals on a `string` receiver — `FirstOrNull()` /
// `LastOrNull()` / `SingleOrNull()` (no-arg AND predicate forms) — were
// admitted by the sema-level string-receiver LINQ exemption gate
// (`common::isStringLinqOperatorMemberName`,
// `src/common/BuiltinRegistry.cpp`'s `kStringLinqOperatorMemberNames`, 39
// names — FirstOrNull/LastOrNull/SingleOrNull have been on this list since
// the #3589 table consolidation). However, the codegen arm of those four
// sites (`LlvmBackend_Calls.cpp`, `Emitter::tryLowerCollectionInstanceCall`)
// derived the element TypeRef as `recvTy->genericArgs[0]` — `nullptr` for a
// `string` receiver (its `recvTy` is the bare `string` Primitive TypeRef,
// with no `genericArgs`; LinkedList<T>/Range<T>/vtable-IEnumerable<T> all
// carry a real `genericArgs[0]`). That left `isValTy = false` (the
// "pointer-typed element" branch), and the terminal returned a BARE i32 for
// the `char?` value. Force-unwrapping with `!` (sema types the local as
// `char?` = `{i8 hasValue; i32}`, fix #849) then SEGFAULTED the compiler,
// reading garbage from the struct fields.
//
// This is the SAME shape issue #3786 measured for `MinOrNull`/`MaxOrNull`
// (and the same fix: when `isStringRecvTerm` is true, substitute
// `ctx_.primitiveTypeRef(sema::PrimitiveType::Char)` for `elemRefNN` — the
// same `char` TypeRef `lowerToLazyEnumerator`'s own string arm already
// builds, `LlvmBackend_Calls3.cpp`, fix #1220). #3786 explicitly declined
// to touch this scope ("issue 3795 filed with a measured repro", per the
// #3786 commit body) — that is the issue this file ships.
//
// The fix: `src/codegen/LlvmBackend_Calls.cpp`, all FOUR `*OrNull` arms
// (FirstOrNull/LastOrNull/SingleOrNull × {no-arg, predicate}): substitute
// `ctx_.primitiveTypeRef(sema::PrimitiveType::Char)` for `elemRefNN` when
// `isStringRecvTerm` is true. No other arm touched; `recv`/`esz`/the
// materialize path are exactly the same as First/Sum/Min/Max/etc. already
// take on a string receiver (issue #2783 array arm + #3764's isArrTerminal
// + #1220's string arm).
//
// Comparator families:
//   non-empty  -> First/Last/SingleOrNull return a non-null char; FirstOrNull(pred)
//                returns the matching char (or null on no match)
//   empty      -> FirstOrNull/LastOrNull/SingleOrNull return null
//   predicate  -> FirstOrNull(pred)/SingleOrNull(pred)/LastOrNull(pred) per
//                stdlib §5: FirstOrNull(pred) = first matching, null on none;
//                SingleOrNull(pred) = the unique match, null on zero/many;
//                LastOrNull(pred) = last matching, null on none.
//   non-ASCII  -> code-point comparison (matching what First()/Min() already
//                do on a string per §9.6 / fix #1220), not UTF-8 bytes.
//
// Spec citation: stdlib §5.1 (element terminals; FirstOrNull/LastOrNull/
// SingleOrNull/predicate variants; "the *OrNull variants return null
// instead [of throwing]" — `string` receiver) + §9.6 (string iterates as
// `IEnumerable<char>` per fix #1220, code-point not UTF-8 byte).
// Spec revision: v0.286 (this fix pins the §5.1 rule at this revision).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        body + "\n    return 0;\n  end Main;\nend C;\n";
}

}  // namespace

// ===========================================================================
// The issue's OWN repro (GH #3795): force-unwrap with `!` on every form.
// On the pre-fix tree, all four rows below SEGFAULTED `bezelc` at compile
// time (rc=139 SIGSEGV, confirmed on this lane's HEAD before the patch).
// With the fix, they print the expected code points.
// ===========================================================================

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           issue_repro_first_or_null_force_unwrap) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"hello\";\n"
            "    var c := s.FirstOrNull()!;\n"
            "    Console.WriteLine(int(c));")),
        "104");
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           issue_repro_last_or_null_force_unwrap) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"hello\";\n"
            "    var c := s.LastOrNull()!;\n"
            "    Console.WriteLine(int(c));")),
        "111");
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           issue_repro_single_or_null_force_unwrap) {
    // "hello" has 5 elements -> SingleOrNull returns null -> `!` on null
    // throws NullReferenceException (the documented stdlib §5 behavior for
    // force-unwrapping a null T?; the segfault-pre-fix tree died BEFORE
    // reaching this point, so this row also discriminates against the
    // pre-fix binary). Single-element case follows.
    auto r = runAot(prog(
        "    var s := \"hello\";\n"
        "    var c := s.SingleOrNull()!;\n"
        "    Console.WriteLine(int(c));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);  // NullReferenceException
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           issue_repro_first_or_null_predicate_force_unwrap) {
    // FirstOrNull(pred) where pred matches 'l' (108): first 'l' is index 2.
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"hello\";\n"
            "    var c := s.FirstOrNull(lambda(x: char) => x = 'l')!;\n"
            "    Console.WriteLine(int(c));")),
        "108");
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           issue_repro_single_or_null_predicate_force_unwrap) {
    // SingleOrNull(pred) where pred matches exactly one char: 'l' appears
    // twice in "hello" (index 2, 3) -> null -> `!` throws.
    auto r = runAot(prog(
        "    var s := \"hello\";\n"
        "    var c := s.SingleOrNull(lambda(x: char) => x = 'l')!;\n"
        "    Console.WriteLine(int(c));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           issue_repro_last_or_null_predicate_force_unwrap) {
    // LastOrNull(pred) where pred matches 'l': last 'l' is index 3 (108).
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"hello\";\n"
            "    var c := s.LastOrNull(lambda(x: char) => x = 'l')!;\n"
            "    Console.WriteLine(int(c));")),
        "108");
}

// ===========================================================================
// §6b-style enumeration: empty/non-empty, no-arg/predicate, the null path
// (the `c = null` discriminator the #3786 commit established as the
// proof oracle for `char` element type — without the elemTypeRef fix, the
// bare-i32 encoding made this print "False" on an empty string instead of
// "True", the SAME latent defect #3786 measured for MinOrNull/MaxOrNull).
// ===========================================================================

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           non_empty_first_last_single_or_null) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"banana\";\n"
            "    Console.WriteLine(int(s.FirstOrNull()!));\n"
            "    Console.WriteLine(int(s.LastOrNull()!));\n"
            "    Console.WriteLine(s.SingleOrNull() = null);")),
        "98\n97\nTrue");
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           empty_returns_null) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"\";\n"
            "    Console.WriteLine(s.FirstOrNull() = null);\n"
            "    Console.WriteLine(s.LastOrNull() = null);\n"
            "    Console.WriteLine(s.SingleOrNull() = null);")),
        "True\nTrue\nTrue");
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           empty_predicate_returns_null) {
    // No char in "" satisfies any predicate -> all three *OrNull(pred) return null.
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"\";\n"
            "    Console.WriteLine(s.FirstOrNull(lambda(x: char) => x = 'l') = null);\n"
            "    Console.WriteLine(s.LastOrNull(lambda(x: char) => x = 'l') = null);\n"
            "    Console.WriteLine(s.SingleOrNull(lambda(x: char) => x = 'l') = null);")),
        "True\nTrue\nTrue");
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           single_char_string_single_or_null_match) {
    // "z" has exactly one element -> SingleOrNull returns it, *OrNull(pred)
    // for a matching predicate returns it, and SingleOrNull(pred) returns
    // it iff pred matches exactly once.
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"z\";\n"
            "    Console.WriteLine(int(s.FirstOrNull()!));\n"
            "    Console.WriteLine(int(s.LastOrNull()!));\n"
            "    Console.WriteLine(int(s.SingleOrNull()!));\n"
            "    Console.WriteLine(int(s.FirstOrNull(lambda(x: char) => x = 'z')!));\n"
            "    Console.WriteLine(int(s.LastOrNull(lambda(x: char) => x = 'z')!));\n"
            "    Console.WriteLine(int(s.SingleOrNull(lambda(x: char) => x = 'z')!));")),
        "122\n122\n122\n122\n122\n122");
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           non_ascii_code_points) {
    // Code-point comparison (§9.6 / fix #1220), not UTF-8 bytes:
    // "aé\U0001F600b" -> chars 97, 233, 128512, 98.
    // FirstOrNull = 'a' (97); LastOrNull = 'b' (98);
    // SingleOrNull(pred x = 'a') = 'a' (97) — exactly one match.
    // FirstOrNull(pred x > 'a') = U+00E9 (233); LastOrNull(pred x > 'a') = 'b' (98).
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"a\\u00E9\\U0001F600b\";\n"
            "    Console.WriteLine(int(s.FirstOrNull()!));\n"
            "    Console.WriteLine(int(s.LastOrNull()!));\n"
            "    Console.WriteLine(int(s.SingleOrNull(lambda(x: char) => x = 'a')!));\n"
            "    Console.WriteLine(int(s.FirstOrNull(lambda(x: char) => x > 'a')!));\n"
            "    Console.WriteLine(int(s.LastOrNull(lambda(x: char) => x > 'a')!));")),
        "97\n98\n97\n233\n98");
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           chain_where_then_first_or_null) {
    // "banana" > 'a': chars at indices 1..5 (b, n, n, n, n).
    // FirstOrNull = 'b' (98); LastOrNull = 'n' (110).
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"banana\";\n"
            "    var f := s.Where(lambda(c: char) => c > 'a').FirstOrNull();\n"
            "    var l := s.Where(lambda(c: char) => c > 'a').LastOrNull();\n"
            "    Console.WriteLine(int(f!));\n"
            "    Console.WriteLine(int(l!));")),
        "98\n110");
}

// Control: the fix touches the four `*OrNull` arms ONLY. Other string-
// receiver LINQ terminals (First/Last/Min/Max) are unaffected
// (byte-identical dispatch — this fix only changes the `elemRefNN`
// derivation when `isStringRecvTerm`). "banana" = b(98) a(97) n(110)
// a(97) n(110) a(97); First='b' (98), Last='a' (97). Single() throws on
// a multi-element receiver (stdlib §5: Single() returns the single
// element or throws InvalidOperationException), so we exercise First /
// Last only here.
BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           other_string_terminals_unaffected) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"banana\";\n"
            "    Console.WriteLine(int(s.First()));\n"
            "    Console.WriteLine(int(s.Last()));")),
        "98\n97");
}

// ===========================================================================
// Leak-amplified (lane rules §R12-G / §3): the materialized char-array
// temporary (`bz_str_to_char_array` + `bz_lazy_to_array`) is freed exactly
// once per terminal call (`freeLazyTemp()`), before the call returns to
// the loop body — a leaked or double-freed snapshot shows up under
// ASan/LSan over 200 iterations. `runAotChecked` is ASan/LSan-instrumented
// independently of the plain-oracle gtest run.
// ===========================================================================

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           leak_free_200x_force_unwrap) {
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<200 do\n"
        "      var s := \"banana\";\n"
        "      var f := s.FirstOrNull()!;\n"
        "      var l := s.LastOrNull()!;\n"
        "      total := total + int(f) + int(l);\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    // "banana" First = 'b' (98), Last = 'a' (97); (98 + 97) * 200 = 39000.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=39000\n"));
}

BEZEL_TEST(firstor_null_lastor_null_singleor_null_string_receiver_3795,
           leak_free_200x_empty_string_null_path) {
    auto r = runAotChecked(prog(
        "    var nullCount := 0;\n"
        "    for var i in 0..<200 do\n"
        "      var s := \"\";\n"
        "      var f := s.FirstOrNull();\n"
        "      var l := s.LastOrNull();\n"
        "      if f = null and l = null then\n"
        "        nullCount := nullCount + 1;\n"
        "      end;\n"
        "    end;\n"
        "    Console.WriteLine($\"nullCount={nullCount}\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nullCount=200\n"));
}