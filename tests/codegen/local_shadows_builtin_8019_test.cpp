// Regression tests for #8019 — a LOCAL binding that shadows a BUILT-IN's
// TYPE name must WIN over the built-in's text-keyed static arm.
//
// THE DEFECT (the issue's verbatim repro).  A local variable whose name is a
// built-in's TYPE name still routed the call to the built-in's text-keyed
// static arm, so the user's own method was never called:
//
//     var Path := new Combiner();
//     Console.WriteLine(Path.Combine("x", "y"));
//
// MEASURED pre-fix (the #8019 base `f7f2cab6`, `build/bezelc`): the program
// compiled, exited 0, and printed `x/y` — the BUILT-IN `Path.Combine` ran and
// the user's `USER:x+y` never did.  A SILENT wrong answer.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * Spec §2.3 (v0.355, *Always-available names*), the paragraph after the
//     table: "Only the first row's names cannot be shadowed (§3.4); every
//     other always-available name may be shadowed by local declarations under
//     the normal name-resolution rules, and stands in those rules where an
//     imported bare name does (§2.7 rule 1)".
//   * Spec §2.3's table: the FIRST row — "Primitive value types, their
//     reserved PascalCase aliases, and `string`/`String`" — is the only one
//     marked "**No**" (not shadowable).  `Path`, `Math`, `Convert`,
//     `Environment`, `DateTime`, `TimeSpan`, `Guid`, `Random`, `File`,
//     `Directory`, `Debug`, `IntPtr`, `UIntPtr`, `CancellationToken` are all
//     in the shadowable rows (`System`, `System.IO`, `System.Diagnostics`).
//   * Spec §2.7 rule 1 (v0.355, *Name resolution for dotted names*):
//     "Resolve the leftmost segment `A` as a bare name: local scope, then the
//     enclosing type's accessible members … If `A` resolves to a *type*, the
//     rest of the chain resolves against that type's members … If it resolves
//     to a *value* (a local, field, or property), the chain is member access."
//
// So a LOCAL (or a parameter, or a field, or a property, or a `for` variable,
// or a pattern binding, or a captured closure local) named `Path` resolves to a
// VALUE, and `Path.Combine(…)` is MEMBER ACCESS on it — the user's own
// method.  Only §3.4's reserved row may not be shadowed.
//
// THE FIX.  The text-keyed ladders ask "does this receiver's TEXT name a
// built-in?", and #5272 / #5272-ff answered that with "is the receiver's
// symbol a USER-DECLARED type?" — which cannot tell a user-declared TYPE from
// a VALUE, because a local's symbol is neither.  The shared predicate is now
// `sema::receiverMayReachBuiltinStaticArm` (src/sema/Symbol.hpp): the head of
// a built-in static arm may be reached only when it names the BUILT-IN class
// (the synthetic `builtinClassSym`, shaped `decl == nullptr &&
// moduleName.empty()`).  SEMA's static-member TYPING and CODEGEN's static
// CALL and static PROPERTY lowering all gate on it, so the two layers cannot
// disagree about what a shadowing receiver means.
//
// THE ORACLE.  stdout byte-for-byte and exit 0.  Every user row asserts the
// USER's bytes: a `didNotCrash` assertion would pass against the defect, and
// several of the pre-fix shapes were not even a wrong value — `Math.Sqrt`,
// `Random.Next` and `Debug.Assert` on a shadowing local produced a MALFORMED
// IR ("Call parameter type does not match function signature!"), i.e. a
// compile error, so those rows are red on the base for a second reason too.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The USER's own class.  Its members are spelled like the built-in members
// the arms key on, so a receiver that resolves to a VALUE reaches the USER's
// member.  `Combine` takes TWO arguments because the built-in `Path.Combine`
// does — that arity is what let the built-in arm fire on the user's call.
const char* kSh =
    "public class Sh "
    "  public method Combine(a: string, b: string): string "
    "    return \"USER:\" + a + \"+\" + b; "
    "  end Combine; "
    "  public method Only(a: string): string "
    "    return \"ONLY\"; "
    "  end Only; "
    "  public method Sqrt(d: double): double "
    "    return 7.0; "
    "  end Sqrt; "
    "  public method ToInt32(s: string): int "
    "    return 99; "
    "  end ToInt32; "
    "  public method Next(n: int): int "
    "    return 5; "
    "  end Next; "
    "  public method Exists(p: string): bool "
    "    return true; "
    "  end Exists; "
    "  public method Assert(c: bool): int "
    "    return 1; "
    "  end Assert; "
    "  public method NewGuid(): string "
    "    return \"USERGUID\"; "
    "  end NewGuid; "
    "  public property Pi: double => 3.25; "
    "  public property ProcessorCount: int => 555; "
    "  public property Now: int => 111; "
    "  public property Empty: int => 777; "
    "  public property Zero: int => 444; "
    "  public property None: int => 888; "
    "end Sh; ";

// `classes` are module-level declarations; `members` are extra members of the
// program class (a helper method, a field, a property); `body` is Main's body.
std::string prog(const std::string& classes, const std::string& members,
                 const std::string& body) {
    return std::string("module M; ") + classes +
           " public class C " + members +
           " public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

void expectOut(const std::string& classes, const std::string& members,
               const std::string& body, const std::string& want) {
    auto r = runAot(prog(classes, members, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// A compile REJECT: the row asserts WHICH rule fired, not merely that
// something did (issue 2988).
bool hasErrorContaining(const auto& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// #8019: the LOCAL half — a shadowing binding wins over the built-in arm.
// ---------------------------------------------------------------------------

// The issue's own repro, verbatim: a local `Path` and the user's own
// `Combine`.  Pre-fix: `x/y` (the built-in `Path.Combine`).
BEZEL_TEST(local_shadows_builtin_8019, path_local_var_calls_the_user_method) {
    expectOut(kSh, "",
              "var Path := new Sh(); "
              "Console.WriteLine(Path.Combine(\"x\", \"y\"));",
              "USER:x+y\n");
}

// The same binding in the other POSITIONS a value can be introduced in.
// §2.7 rule 1 lists "a local, field, or property"; a parameter, a `for`
// variable, a pattern binding and a captured closure local are the same
// "resolves to a value" case.

// A PARAMETER.
BEZEL_TEST(local_shadows_builtin_8019, path_parameter_calls_the_user_method) {
    expectOut(kSh,
              "public static method Go(Path: Sh): string "
              "  return Path.Combine(\"x\", \"y\"); "
              "end Go; ",
              "Console.WriteLine(Go(new Sh()));",
              "USER:x+y\n");
}

// A LAMBDA-CAPTURED LOCAL.
BEZEL_TEST(local_shadows_builtin_8019,
           path_lambda_captured_local_calls_the_user_method) {
    expectOut(kSh, "",
              "var Path := new Sh(); "
              "var f := lambda(): string => Path.Combine(\"x\", \"y\"); "
              "Console.WriteLine(f());",
              "USER:x+y\n");
}

// A `for` LOOP VARIABLE.  The collection is the OWNING family: §15.4 /
// §28.11 reject a fresh class-typed temporary stored into a borrowing
// `Array` ("this `.Add(...)` store … stores a fresh unbound owned temporary
// into a borrowing-family collection"), so the row's program uses
// `OwningArray` — the point of the row is the loop VARIABLE's name, not the
// collection's ownership family.
BEZEL_TEST(local_shadows_builtin_8019, path_for_variable_calls_the_user_method) {
    expectOut(kSh, "",
              "using var arr := new OwningArray<Sh>(); arr.Add(new Sh()); "
              "for var Path in arr do "
              "  Console.WriteLine(Path.Combine(\"x\", \"y\")); "
              "end for;",
              "USER:x+y\n");
}

// A PATTERN BINDING.
BEZEL_TEST(local_shadows_builtin_8019, path_pattern_binding_calls_the_user_method) {
    expectOut(kSh, "",
              "var o: Object := new Sh(); "
              "case o of "
              "  var Path: Sh => Console.WriteLine(Path.Combine(\"x\", \"y\")), "
              "  _ => Console.WriteLine(\"other\"); "
              "end;",
              "USER:x+y\n");
}

// A bare FIELD — §2.7 rule 1's value list names "a local, field, or
// property", and the enclosing type's accessible members are the second
// position of that rule's resolution order.
BEZEL_TEST(local_shadows_builtin_8019, path_bare_field_calls_the_user_method) {
    expectOut(kSh,
              "public owned Path: Sh := new Sh(); "
              "public method Run(): string "
              "  return Path.Combine(\"x\", \"y\"); "
              "end Run; ",
              "var p := new C(); Console.WriteLine(p.Run());",
              "USER:x+y\n");
}

// A bare PROPERTY — the third kind of value §2.7 rule 1 names.
BEZEL_TEST(local_shadows_builtin_8019,
           path_bare_property_calls_the_user_method) {
    expectOut(kSh,
              "private owned _s: Sh := new Sh(); "
              "public property Path: Sh => _s; "
              "public method Run(): string "
              "  return Path.Combine(\"x\", \"y\"); "
              "end Run; ",
              "var p := new C(); Console.WriteLine(p.Run());",
              "USER:x+y\n");
}

// The same local, but reaching a member the built-in does NOT have: the
// built-in arm has no such member at all, so this row pins that the call
// really is the user's own (and not a coincidence of the member's name).
BEZEL_TEST(local_shadows_builtin_8019, path_local_var_calls_a_user_only_member) {
    expectOut(kSh, "",
              "var Path := new Sh(); "
              "Console.WriteLine(Path.Only(\"x\"));",
              "ONLY\n");
}

// ---- the other spellings of the family, in the CALL form ------------------
//
// Each of these is a distinct `cls == "<Builtin>"` arm of the static-CALL
// ladder, so a fix that gated only `Path` would be caught here.

// `Math.Sqrt` — pre-fix this was a MALFORMED IR ("Call parameter type does
// not match function signature!"), because the built-in arm's own `double`
// argument met the user's `string`.
BEZEL_TEST(local_shadows_builtin_8019, math_local_var_calls_the_user_method) {
    expectOut(kSh, "",
              "var Math := new Sh(); "
              "Console.WriteLine(Math.Sqrt(2.0));",
              "7\n");
}

// `Convert.ToInt32`.
BEZEL_TEST(local_shadows_builtin_8019, convert_local_var_calls_the_user_method) {
    expectOut(kSh, "",
              "var Convert := new Sh(); "
              "Console.WriteLine(Convert.ToInt32(\"42\"));",
              "99\n");
}

// `Random.Next` — the other MALFORMED-IR row.
BEZEL_TEST(local_shadows_builtin_8019, random_local_var_calls_the_user_method) {
    expectOut(kSh, "",
              "var Random := new Sh(); "
              "Console.WriteLine(Random.Next(5));",
              "5\n");
}

// `File.Exists`.
BEZEL_TEST(local_shadows_builtin_8019, file_local_var_calls_the_user_method) {
    expectOut(kSh, "",
              "var File := new Sh(); "
              "Console.WriteLine(File.Exists(\"x\"));",
              "True\n");
}

// `Debug.Assert` — the third MALFORMED-IR row.
BEZEL_TEST(local_shadows_builtin_8019, debug_local_var_calls_the_user_method) {
    expectOut(kSh, "",
              "var Debug := new Sh(); "
              "Console.WriteLine(Debug.Assert(true));",
              "1\n");
}

// `Guid.NewGuid` — the CALL form of the `Guid` spelling.
BEZEL_TEST(local_shadows_builtin_8019, guid_local_var_calls_the_user_method) {
    expectOut(kSh, "",
              "var Guid := new Sh(); "
              "Console.WriteLine(Guid.NewGuid());",
              "USERGUID\n");
}

// ---- the other spellings of the family, in the PROPERTY form --------------
//
// These are the static-PROPERTY-read arms, and for several of them SEMA
// typed the read as the built-in's too — so a codegen-only fix would leave
// the two layers disagreeing.

// `Math.Pi` — pre-fix: 3.141592653589793 (the built-in's π).
BEZEL_TEST(local_shadows_builtin_8019, math_local_var_property_read) {
    expectOut(kSh, "",
              "var Math := new Sh(); Console.WriteLine(Math.Pi);",
              "3.25\n");
}

// `Environment.ProcessorCount` — pre-fix: 20 (the host's real nproc).
BEZEL_TEST(local_shadows_builtin_8019,
           environment_local_var_property_read) {
    expectOut(kSh, "",
              "var Environment := new Sh(); "
              "Console.WriteLine(Environment.ProcessorCount);",
              "555\n");
}

// `DateTime.Now` — pre-fix: the built-in's clock.
BEZEL_TEST(local_shadows_builtin_8019, datetime_local_var_property_read) {
    expectOut(kSh, "",
              "var DateTime := new Sh(); Console.WriteLine(DateTime.Now);",
              "111\n");
}

// `Guid.Empty` — pre-fix: the built-in's all-zero Guid.
BEZEL_TEST(local_shadows_builtin_8019, guid_local_var_property_read) {
    expectOut(kSh, "",
              "var Guid := new Sh(); Console.WriteLine(Guid.Empty);",
              "777\n");
}

// `IntPtr.Zero` — pre-fix: 0 (the null pointer).  `IntPtr` carries no value
// symbol, so this row also pins that an UNSTAMPED receiver keeps working
// when nothing shadows it (the control below).
BEZEL_TEST(local_shadows_builtin_8019, intptr_local_var_property_read) {
    expectOut(kSh, "",
              "var IntPtr := new Sh(); Console.WriteLine(IntPtr.Zero);",
              "444\n");
}

// `CancellationToken.None` — pre-fix: the built-in's token.
BEZEL_TEST(local_shadows_builtin_8019,
           cancellation_token_local_var_property_read) {
    expectOut(kSh, "",
              "var CancellationToken := new Sh(); "
              "Console.WriteLine(CancellationToken.None);",
              "888\n");
}

// ---------------------------------------------------------------------------
// Controls: the built-ins still lower when NOTHING shadows them.
// ---------------------------------------------------------------------------

// Without these the fix could pass by simply never reaching a built-in arm.

BEZEL_TEST(local_shadows_builtin_8019, builtin_path_combine_still_lowers) {
    expectOut("", "", "Console.WriteLine(Path.Combine(\"x\", \"y\"));", "x/y\n");
}

BEZEL_TEST(local_shadows_builtin_8019, builtin_math_sqrt_still_lowers) {
    expectOut("", "", "Console.WriteLine(Math.Sqrt(2.0));",
              "1.4142135623730951\n");
}

BEZEL_TEST(local_shadows_builtin_8019, builtin_math_pi_still_lowers) {
    expectOut("", "", "Console.WriteLine(Math.Pi);", "3.141592653589793\n");
}

BEZEL_TEST(local_shadows_builtin_8019, builtin_convert_to_int32_still_lowers) {
    expectOut("", "", "Console.WriteLine(Convert.ToInt32(\"42\"));", "42\n");
}

BEZEL_TEST(local_shadows_builtin_8019, builtin_guid_empty_still_lowers) {
    expectOut("", "", "Console.WriteLine(Guid.Empty);",
              "00000000-0000-0000-0000-000000000000\n");
}

BEZEL_TEST(local_shadows_builtin_8019, builtin_intptr_zero_still_lowers) {
    expectOut("", "", "Console.WriteLine(IntPtr.Zero);", "0\n");
}

// The built-in `DateTime.Now` is a tick count, so it is machine- and
// run-dependent: assert the SHAPE (a huge positive count) rather than a
// literal, so the control cannot go stale and still separates the two
// lowerings (the user's `Now` is the small value 111).
BEZEL_TEST(local_shadows_builtin_8019, builtin_datetime_now_still_lowers) {
    expectOut("", "",
              "var n: long := DateTime.Now.Ticks; Console.WriteLine(n > 1000000000000L);",
              "True\n");
}

// The built-in `Environment.ProcessorCount` is the host's real nproc: assert
// that it is positive AND that it is not the user's 555.
BEZEL_TEST(local_shadows_builtin_8019,
           builtin_environment_processor_count_still_lowers) {
    expectOut("", "",
              "var n: int := Environment.ProcessorCount; "
              "Console.WriteLine(n > 0); Console.WriteLine(n = 555);",
              "True\nFalse\n");
}

// A local whose name is NOT a built-in's is untouched — the false-positive
// control: the guard must key on the RESOLVED SYMBOL, not on the spelling.
BEZEL_TEST(local_shadows_builtin_8019, non_builtin_name_is_unaffected) {
    expectOut(kSh, "",
              "var Widget := new Sh(); "
              "Console.WriteLine(Widget.Combine(\"x\", \"y\"));",
              "USER:x+y\n");
}

// A member of a USER class named after a built-in is untouched — #5272's
// row, kept so this file's suite pins both halves of the family.
BEZEL_TEST(local_shadows_builtin_8019, user_class_shadow_is_unaffected) {
    expectOut("public class Path "
              "  public static method Combine(a: string, b: string): string "
              "    return \"USERCLASS:\" + a + \"+\" + b; "
              "  end Combine; "
              "end Path; ",
              "",
              "Console.WriteLine(Path.Combine(\"x\", \"y\"));",
              "USERCLASS:x+y\n");
}

// A FIRST-ROW reserved name (§3.4) still cannot be shadowed: the lowercase
// keyword alias `int` is a reserved word (Appendix B.1), so the declaration
// is REFUSED.  This is the control for the "only §3.4's row is reserved"
// half of §2.3 — the fix must not widen what a local may be called.
//
// (The PascalCase aliases — `String`, `Int32`, `Boolean`, `Char`, `Double` —
// are §3.4-reserved too, but the compiler ACCEPTS `var String := 5;` today;
// that gap is another lane's, so this row uses the keyword spelling, which
// the lexer refuses on its own.)
BEZEL_TEST(local_shadows_builtin_8019, reserved_keyword_name_is_still_refused) {
    auto r = runAot("module M; public class C public static method Main(): int "
                    "var int := 5; Console.WriteLine(int); "
                    "return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "reserved keyword"));
}
