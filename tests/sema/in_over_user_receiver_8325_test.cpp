// #8325 (§17.5, §7.4, §26.5, §13.1, §22.12, v0.355): `x in c` over a
// USER-DECLARED receiver — the §17.5 STRUCTURAL `Contains` lookup, and the
// invalid shapes' rejection at the `in` SITE.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §17.5 (v0.355) — the `in` operator:
//   "`x in collection` evaluates to `true` if `collection.Contains(x)` is
//    true. The right operand must expose a `Contains(T): bool` method
//    (instance or extension)."
//   "The `Contains` lookup is structural rather than interface-based: any
//    accessible method named `Contains` taking one parameter of compatible
//    type and returning `bool` qualifies, whether it is an instance member
//    or an extension method (§26)."
//
// spec §26.5 (v0.355) — the resolution ORDER the lookup follows:
//   rule 1 — "If `x`'s declared type (or any base type) *declares* any
//     instance member named `Foo` — of any arity, applicability, or
//     accessibility — instance resolution is committed and extension blocks
//     are never consulted for that name on that receiver."
//   rule 2 — otherwise the extension blocks are searched.
//
// spec §7.4 (v0.355) — a member access on a NULLABLE value requires
// narrowing first (`x <> null` / `??` / `!`), and the narrowing is
// flow-sensitive: "Narrowing is established by … a null test in either
// spelling … an early exit … assignment … and from a declaration whose
// initializer is such an `expr` — `var x: T? := expr;`".
//
// spec §13.1 (v0.355) — the structural lookup qualifies an ACCESSIBLE member
// only, so a `private Contains` reached from another class does not qualify.
//
// WHAT WAS WRONG (MEASURED on r30-c1 = 23cb63f8, the base of this lane).
//
//   not yet implemented: codegen for this `in` expression form
//
// — for NINE of the TEN probes of issue #8325, and the message is a CODEGEN
// "I did not implement this" message: the operator's shape was decided only
// in the backend, and only for a receiver whose static type is a built-in
// collection interface. So a user-declared receiver reached the backend for
// BOTH the valid shapes and the invalid ones:
//   (A) VALID per §17.5, wrongly refused — a generic `Bag<T>` with
//       `Contains(item: T): bool`, an EXTENSION `Contains`, and an
//       interface-typed receiver (`var b: IBag := new Bag()`);
//   (B) INVALID per §17.5, rejected at the WRONG LAYER — no `Contains` at
//       all, a `Contains` returning `int`, a `Contains` taking two
//       parameters, a `static Contains`, and a NULLABLE receiver.
//
// §17.5 decides both questions, so both are decided in SEMA, at the `in`
// site: the valid shapes are admitted (and lower as the very call
// `c.Contains(x)`), and the invalid ones get a diagnostic that NAMES the
// rule they break.
//
// THE ROWS.
//
//  (A) The valid user-receiver shapes COMPILE and answer the DATA:
//      a generic `Bag<string>`, an extension `Contains`, an interface-typed
//      receiver, and the plain non-generic class the lookup generalises.
//  (B) The invalid shapes are COMPILE ERRORS whose diagnostic is at the
//      `in` SITE and NAMES the rule: §17.5's structural lookup (no
//      `Contains`, a non-`bool` one, a two-parameter one, a `static` one,
//      a `private` one) and §7.4's narrowing remedies (a nullable receiver
//      that is not narrowed).
//  (C) A NULLABLE receiver that §7.4 HAS narrowed stays admitted — the
//      written call's own verdict (§17.5: `x in c` IS `c.Contains(x)`),
//      whether the narrowing came from an explicit null test or from the
//      declaration's initializer (§7.4: "from a declaration whose initializer
//      is such an `expr` — `var x: T? := expr;` — which is the
//      declaration's assignment").
//  (D) The CONTROL: the built-in collection receivers keep their verdicts
//      byte-for-byte (the `in` operator over `Array` / `Dictionary` / `Set`
//      / a range / a `string` is unchanged).
//
// ORACLE NOTES.
//
//   * The DATA rows drive the real AOT path (fork-isolated
//     `Driver::compileFile` + clang link + native exec) and read the
//     program's DATA (the membership verdicts), not a constant.
//   * The REJECT rows use TWO oracles, because the defect is a LAYER
//     defect: the SEMA oracle (`parseAndTypecheckGated`) reads the
//     diagnostic's own structure — its text and the LINE it is reported at
//     (the `in` site's line) — and the AOT oracle reads the END-TO-END
//     verdict: the compile is refused, the refusal carries the rule, and
//     the backend's "not yet implemented" message is GONE (a diagnostic at
//     the wrong layer is exactly what the fix removes).

#include <string>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::SemaResult;
using bezel::test::parseAndTypecheckGated;
using bezel::test::runAot;

namespace {

// The first ERROR diagnostic carrying `needle`, or null.
const bezel::Diagnostic* findSemaError(const SemaResult& r,
                                       const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return &d;
    return nullptr;
}

// The first ERROR diagnostic carrying `needle` in an AOT run's diagnostics.
const bezel::Diagnostic* findRunError(const JitResult& r,
                                      const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return &d;
    return nullptr;
}

// The backend's "I did not implement this" message for the `in` operator.
// §17.5 decides the operator's shape, so a diagnostic carrying it is a
// diagnostic at the WRONG LAYER — and the rows below pin its absence.
const char* kBackendNyI =
    "not yet implemented: codegen for this `in` expression form";

// ---------------------------------------------------------------------------
// (D) The CONTROL — the built-in collection receivers. Unchanged.
// ---------------------------------------------------------------------------
const char* kBuiltinControl =
    "module P;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var arr := new Array<int>();\n"
    "    arr.Add(3);\n"
    "    var d := new Dictionary<string, int>();\n"
    "    d.Add(\"k\", 1);\n"
    "    var s := new Set<int>();\n"
    "    s.Add(5);\n"
    "    Console.WriteLine(3 in arr);\n"
    "    Console.WriteLine(\"k\" in d);\n"
    "    Console.WriteLine(\"z\" in d);\n"
    "    Console.WriteLine(5 in s);\n"
    "    Console.WriteLine(6 in s);\n"
    "    Console.WriteLine(2 in 1..3);\n"
    "    Console.WriteLine(\"a\" in \"cat\");\n"
    "    Console.WriteLine('a' in \"cat\");\n"
    "    var i := 0;\n"
    "    while not (i in arr) do i := i + 1; end while;\n"
    "    Console.WriteLine(i);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// ---------------------------------------------------------------------------
// (A) The valid user-receiver shapes.
// ---------------------------------------------------------------------------

// A PLAIN non-generic class with an instance `Contains` — the shape the
// structural lookup admits first.
const char* kPlainClassReceiver =
    "module P;\n"
    "public class Bag\n"
    "  public method Contains(item: int): bool return item = 3; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b := new Bag();\n"
    "    Console.WriteLine(3 in b);\n"
    "    Console.WriteLine(4 in b);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// p12: a GENERIC `Bag<T>` with `Contains(item: T): bool`.
const char* kGenericReceiver =
    "module P;\n"
    "public class Bag<T>\n"
    "  private owned _items: Array<T>;\n"
    "  public constructor() _items := new Array<T>(); end constructor;\n"
    "  public method Add(item: T): () _items.Add(item); end Add;\n"
    "  public method Contains(item: T): bool\n"
    "    return _items.Contains(item);\n"
    "  end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b := new Bag<string>();\n"
    "    b.Add(\"a\");\n"
    "    Console.WriteLine(\"a\" in b);\n"
    "    Console.WriteLine(\"b\" in b);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// p18: an EXTENSION `Contains`.
const char* kExtensionReceiver =
    "module P;\n"
    "public class Bag\n"
    "  public method Count(): int return 1; end Count;\n"
    "end Bag;\n"
    "extension Bag\n"
    "  public method Contains(item: int): bool return item = 3; end Contains;\n"
    "end extension;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b := new Bag();\n"
    "    Console.WriteLine(b.Contains(3));\n"
    "    Console.WriteLine(3 in b);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// p25: an INTERFACE-typed receiver.
const char* kInterfaceReceiver =
    "module P;\n"
    "public interface IBag\n"
    "  method Contains(item: int): bool;\n"
    "end IBag;\n"
    "public class Bag: IBag\n"
    "  public constructor() end constructor;\n"
    "  public method Contains(item: int): bool return item = 7; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b: IBag := new Bag();\n"
    "    Console.WriteLine(7 in b);\n"
    "    Console.WriteLine(8 in b);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// ---------------------------------------------------------------------------
// (B) The invalid shapes. Each `in` sits on the line the row asserts.
// ---------------------------------------------------------------------------

// p03: no `Contains` at all.
const char* kNoContains =
    "module P;\n"
    "public class Bag\n"
    "  public method Count(): int return 0; end Count;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b := new Bag();\n"
    "    Console.WriteLine(3 in b);\n"   // line 8 — the `in` site
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// p04: `Contains` returns `int`.
const char* kNonBoolContains =
    "module P;\n"
    "public class Bag\n"
    "  public method Contains(item: int): int return 1; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b := new Bag();\n"
    "    Console.WriteLine(3 in b);\n"   // line 8 — the `in` site
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// p05: `Contains` takes two parameters.
const char* kTwoParameterContains =
    "module P;\n"
    "public class Bag\n"
    "  public method Contains(item: int, other: int): bool return true; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b := new Bag();\n"
    "    Console.WriteLine(3 in b);\n"   // line 8 — the `in` site
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// p07: a `static Contains`.
const char* kStaticContains =
    "module P;\n"
    "public class Bag\n"
    "  public static method Contains(item: int): bool return true; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b := new Bag();\n"
    "    Console.WriteLine(3 in b);\n"   // line 8 — the `in` site
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// p06: a `private Contains`, reached from another class (§13.1 — the
// structural lookup qualifies an ACCESSIBLE member only).
const char* kPrivateContains =
    "module P;\n"
    "public class Bag\n"
    "  private method Contains(item: int): bool return true; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b := new Bag();\n"
    "    Console.WriteLine(3 in b);\n"   // line 8 — the `in` site
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// p34: a NULLABLE receiver that §7.4 has NOT narrowed — a PARAMETER, so
// nothing narrows it (a parameter is narrowed only by a null test, §7.4).
const char* kNullableReceiver =
    "module P;\n"
    "public class Bag\n"
    "  public constructor() end constructor;\n"
    "  public method Contains(item: int): bool return item = 7; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Q(b: Bag?): bool\n"
    "    return 7 in b;\n"               // line 8 — the `in` site
    "  end Q;\n"
    "  public static method Main(): int\n"
    "    Console.WriteLine(\"before\");\n"
    "    Console.WriteLine(Q(null));\n"
    "    Console.WriteLine(\"after\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// ---------------------------------------------------------------------------
// (C) The NULLABLE receivers §7.4 HAS narrowed — both spellings.
// ---------------------------------------------------------------------------

// p26: the receiver is a LOCAL narrowed by its DECLARATION's initializer —
// §7.4: "from a declaration whose initializer is such an `expr` — `var x: T?
// := expr;` — which is the declaration's assignment". The written call
// `b.Contains(7)` is admitted at the base (MEASURED), so §17.5's identity
// (`x in c` IS `c.Contains(x)`) admits the `in` spelling too.
const char* kNarrowedByInitializerReceiver =
    "module P;\n"
    "public class Bag\n"
    "  public constructor() end constructor;\n"
    "  public method Contains(item: int): bool return item = 7; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Main(): int\n"
    "    var b: Bag? := new Bag();\n"
    "    Console.WriteLine(7 in b);\n"   // line 9 — admitted: `b` is narrowed
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The same, narrowed by an explicit null test — the §7.4 remedy the
// diagnostic names.
const char* kNarrowedByNullTestReceiver =
    "module P;\n"
    "public class Bag\n"
    "  public constructor() end constructor;\n"
    "  public method Contains(item: int): bool return item = 7; end Contains;\n"
    "end Bag;\n"
    "class C\n"
    "  public static method Q(b: Bag?): bool\n"
    "    if b <> null then return 7 in b; end if;\n"
    "    return false;\n"
    "  end Q;\n"
    "  public static method Main(): int\n"
    "    var b := new Bag();\n"
    "    Console.WriteLine(Q(b));\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

}  // namespace

// ---------------------------------------------------------------------------
// (D) The CONTROL — the built-in collection receivers keep their verdicts.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_in_over_user_receiver_8325,
           builtin_collection_receivers_still_answer_the_data) {
    JitResult r = runAot(kBuiltinControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "True\nTrue\nFalse\nTrue\nFalse\nTrue\nTrue\nTrue\n3\n");
}

// ---------------------------------------------------------------------------
// (A) The valid user-receiver shapes compile and answer the DATA.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_in_over_user_receiver_8325,
           plain_class_receiver_answers_the_data) {
    JitResult r = runAot(kPlainClassReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           generic_class_receiver_answers_the_data_p12) {
    JitResult r = runAot(kGenericReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           extension_contains_receiver_answers_the_data_p18) {
    JitResult r = runAot(kExtensionReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\n");
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           interface_typed_receiver_answers_the_data_p25) {
    JitResult r = runAot(kInterfaceReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// ---------------------------------------------------------------------------
// (B) The invalid shapes — a SEMA diagnostic at the `in` SITE, naming the
// rule they break. The backend's message must be GONE (§17.5 decides the
// operator's shape, so it is not the backend's question to answer).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_in_over_user_receiver_8325,
           no_contains_is_refused_at_the_in_site_p03) {
    SemaResult sr = parseAndTypecheckGated(kNoContains);
    // §17.5's structural lookup is named, at the `in` site's line.
    const bezel::Diagnostic* d =
        findSemaError(sr, "structural rather than interface-based");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_EQ(d->location.line, 8u);
        BEZEL_EXPECT_TRUE(d->message.find("Contains") != std::string::npos);
    }
    // End to end: refused, with the rule, and NOT at the backend.
    JitResult r = runAot(kNoContains);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        findRunError(r, "structural rather than interface-based") != nullptr);
    BEZEL_EXPECT_FALSE(findRunError(r, kBackendNyI) != nullptr);
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           non_bool_contains_is_refused_at_the_in_site_p04) {
    SemaResult sr = parseAndTypecheckGated(kNonBoolContains);
    // The structural lookup AND the `bool` result requirement are named.
    const bezel::Diagnostic* d =
        findSemaError(sr, "structural rather than interface-based");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_EQ(d->location.line, 8u);
        BEZEL_EXPECT_TRUE(d->message.find("bool") != std::string::npos);
    }
    JitResult r = runAot(kNonBoolContains);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        findRunError(r, "structural rather than interface-based") != nullptr);
    BEZEL_EXPECT_FALSE(findRunError(r, kBackendNyI) != nullptr);
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           two_parameter_contains_is_refused_at_the_in_site_p05) {
    SemaResult sr = parseAndTypecheckGated(kTwoParameterContains);
    // The structural lookup AND the ONE-parameter requirement are named.
    const bezel::Diagnostic* d =
        findSemaError(sr, "structural rather than interface-based");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_EQ(d->location.line, 8u);
        BEZEL_EXPECT_TRUE(d->message.find("ONE") != std::string::npos);
    }
    JitResult r = runAot(kTwoParameterContains);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        findRunError(r, "structural rather than interface-based") != nullptr);
    BEZEL_EXPECT_FALSE(findRunError(r, kBackendNyI) != nullptr);
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           static_contains_is_refused_at_the_in_site_p07) {
    SemaResult sr = parseAndTypecheckGated(kStaticContains);
    // The structural lookup is named, and the `static` member's exclusion
    // (§22.12) with it.
    const bezel::Diagnostic* d =
        findSemaError(sr, "structural rather than interface-based");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_EQ(d->location.line, 8u);
        BEZEL_EXPECT_TRUE(d->message.find("static") != std::string::npos);
    }
    JitResult r = runAot(kStaticContains);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        findRunError(r, "structural rather than interface-based") != nullptr);
    BEZEL_EXPECT_FALSE(findRunError(r, kBackendNyI) != nullptr);
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           private_contains_is_not_accessible_p06) {
    SemaResult sr = parseAndTypecheckGated(kPrivateContains);
    // The structural lookup is named, and §13.1's ACCESSIBILITY with it.
    const bezel::Diagnostic* d =
        findSemaError(sr, "structural rather than interface-based");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_EQ(d->location.line, 8u);
        BEZEL_EXPECT_TRUE(d->message.find("13.1") != std::string::npos);
    }
    JitResult r = runAot(kPrivateContains);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        findRunError(r, "structural rather than interface-based") != nullptr);
    BEZEL_EXPECT_FALSE(findRunError(r, kBackendNyI) != nullptr);
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           nullable_receiver_is_refused_at_the_in_site_p34) {
    SemaResult sr = parseAndTypecheckGated(kNullableReceiver);
    // §7.4's narrowing remedies are named.
    const bezel::Diagnostic* d =
        findSemaError(sr, "requires narrowing it to non-null first");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_EQ(d->location.line, 8u);
        BEZEL_EXPECT_TRUE(d->message.find("\xC2\xA7""7.4") != std::string::npos);
        BEZEL_EXPECT_TRUE(d->message.find("!") != std::string::npos);
    }
    JitResult r = runAot(kNullableReceiver);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        findRunError(r, "requires narrowing it to non-null first") != nullptr);
    BEZEL_EXPECT_FALSE(findRunError(r, kBackendNyI) != nullptr);
}

// ---------------------------------------------------------------------------
// (C) The nullable receivers §7.4 HAS narrowed — the written call's own
// verdict, which §17.5's identity (`x in c` IS `c.Contains(x)`) carries over
// to the operator.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_in_over_user_receiver_8325,
           narrowed_by_declaration_initializer_receiver_is_admitted_p26) {
    SemaResult sr = parseAndTypecheckGated(kNarrowedByInitializerReceiver);
    // §7.4 narrows the local by its declaration's initializer, so the
    // receiver is NOT a nullable operand and there is nothing to refuse.
    BEZEL_EXPECT_FALSE(
        findSemaError(sr, "requires narrowing it to non-null first") !=
        nullptr);
    JitResult r = runAot(kNarrowedByInitializerReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
    BEZEL_EXPECT_FALSE(findRunError(r, kBackendNyI) != nullptr);
}

BEZEL_TEST(sema_in_over_user_receiver_8325,
           narrowed_by_null_test_receiver_is_admitted) {
    JitResult r = runAot(kNarrowedByNullTestReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
    BEZEL_EXPECT_FALSE(findRunError(r, kBackendNyI) != nullptr);
}
