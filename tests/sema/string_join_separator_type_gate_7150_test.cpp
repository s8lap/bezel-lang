// fix #7150 — stdlib §9.6 declares `string.Join`'s SEPARATOR as a `string`,
// but the compiler admitted a `char` separator, lowered it as a string
// pointer, and emitted MALFORMED IR — the COMPILER'S OWN IR verifier then
// rejected the module.
//
// THE DEFECT. stdlib §9.6 (v0.343) declares
//
//   public static method Join(separator: string, items: IEnumerable<string>): string;
//
// — the separator is a `string`, and there is NO `char` separator overload.
// §5.1's implicit-conversion table has no row into `string` at all (its
// `char` row widens to `int`/`uint`/`long`/`ulong`/`float`/`double`/
// `decimal`, never to `string`), and language spec §4.6 says it outright:
// "The compiler does not implicitly convert other types to `string`." So a
// `char` separator is not an admissible argument.
//
// A built-in `string` member is a compiler intrinsic: it has no MethodSymbol,
// so §22.5's per-argument assignability check never ran for this call, and
// the codegen lowering (`LlvmBackend_Calls.cpp`'s static `Join` arm) lowered
// `call.args[0]` — the separator — as a `bz_string*` whatever its type.
//
// MEASURED pre-fix on this tree (each row = the call, the observed result):
//
//   `string.Join(',', parts)`   → the compiler emits MALFORMED IR: "Load
//     operand must be a pointer." x3, then "codegen error: IR verification
//     failed (malformed IR emitted)" — the `char` arrives as an `i32` where
//     a pointer is expected;
//   `string.Join(5, parts)` and `string.Join(sep, parts)` over a `char` /
//     `double` variable → the same malformed IR ("GEP base pointer is not a
//     vector or a vector of pointers" + "Load operand must be a pointer.");
//   `String.Join(',', parts)` → the same malformed IR (the PascalCase alias
//     of the same type keyword);
//   `string.Join(null, parts)` → compiled CLEANLY and then SEGFAULTED at run
//     time (the lowering reads the length and the data pointer out of a null
//     string header).
//
// ⇒ Every one of those is a program the document does not define, and each
// was a COMPILER defect (a malformed module, an ICE) rather than a
// diagnostic. The fix REJECTS the call in sema — §9.6 declares the
// separator's type, §5.1/§4.6 decide what converts into it — so the
// lowering never sees an argument it has no lowering for.
//
// WHERE THE REJECTION BELONGS. In SEMA: the document says the call is not
// well-formed, so the rejection belongs where the call is CHECKED. Codegen
// is unreachable for a rejected program (`Driver::compileFile` runs it only
// when `!diagnostics.hasErrors()`), so the +1 producer recognizer in
// `LlvmBackend_Setup.cpp` — which claims `string.Join` as an `rc := 1`
// heap-string producer — stays CONSISTENT with the rejection by
// construction: a rejected call is never classified.
//
// THE CONTROLS ARE THE POINT. This is a gate ADDED to a parameter, so the
// live risk is over-rejection of the separators §9.6 DOES declare: a
// `string` separator, and a `string?` that §7.4 has narrowed non-null (the
// narrowed read HAS type `string`). Every control asserts a VALUE, never
// "it compiled".

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A bare `module M; class C ... end C;` shell — the string members are
// compiler-intrinsic, so no imports or hierarchy are required.
std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// The body every REJECT row shares: two parts, so a separator that IS used
// (rather than skipped for want of a second element) is observable.
const char* kParts =
    "    using var parts := new Array<string>();\n"
    "    parts.Add(\"ab\");\n"
    "    parts.Add(\"cd\");\n";

// True iff SOME error diagnostic is the §5.1 "not implicitly convertible"
// wording for the `string.Join` separator.
bool notConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("the separator argument to `string.Join` has a "
                           "type that is not implicitly convertible to the "
                           "target type (§5.1)") != std::string::npos)
            return true;
    return false;
}

// True iff SOME error diagnostic is the §7 nullability wording (the rule for
// a `null` separator, and for a `string?` that §7.4 has NOT narrowed).
bool nullableSeparator(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("the separator argument to `string.Join` is "
                           "nullable but the target type is non-nullable "
                           "(§7)") != std::string::npos)
            return true;
    return false;
}

// True iff the pre-existing §9.6 ARITY diagnostic is present (the gate this
// fix must NOT disturb — #7140's, which keys on the argument COUNT).
bool arityDiagnostic(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("the call to `string.Join` supplies") !=
                std::string::npos &&
            d.message.find("stdlib §9.6 declares") != std::string::npos)
            return true;
    return false;
}

// The rejection must be a REAL front-end rejection at a real location — not
// a crash, not a hang, not an IR-verification failure. (A compiler that
// dies is the defect this issue is about.)
bool rejectedAtARealLocation(const JitResult& r) {
    return bezel::test::failedAtCompileTimeWithRealLocation(r);
}

}  // namespace

// ── REJECT — a `char` separator (the issue's literal repro) ───────────────
// MEASURED pre-fix: the compiler emitted malformed IR and died with "Load
// operand must be a pointer." x3 + "IR verification failed (malformed IR
// emitted)".
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           reject_char_literal_separator) {
    auto r = runAot(prog(std::string(kParts) +
                         "    Console.WriteLine(string.Join(',', parts));\n" +
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// The same, over a `char` VARIABLE — the crash does not need a literal.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           reject_char_variable_separator) {
    auto r = runAot(prog(std::string(kParts) +
                         "    var sep: char := ',';\n"
                         "    Console.WriteLine(string.Join(sep, parts));\n" +
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// MEASURED pre-fix: the same malformed IR.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           reject_int_separator) {
    auto r = runAot(prog(std::string(kParts) +
                         "    Console.WriteLine(string.Join(5, parts));\n" +
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// MEASURED pre-fix: the same malformed IR ("GEP base pointer is not a
// vector or a vector of pointers").
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           reject_double_separator) {
    auto r = runAot(prog(std::string(kParts) +
                         "    var sep: double := 1.5;\n"
                         "    Console.WriteLine(string.Join(sep, parts));\n" +
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// The PascalCase alias of the same type keyword reaches the same lowering,
// so it must be rejected the same way. MEASURED pre-fix: the same malformed
// IR.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           reject_pascalcase_string_char_separator) {
    auto r = runAot(prog(std::string(kParts) +
                         "    Console.WriteLine(String.Join(',', parts));\n" +
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// A `null` separator — §9.6 declares a NON-NULLABLE `string`, and §7
// rejects a nullable value at a non-nullable target.
//
// ⚠ THIS IS A BEHAVIOUR FLIP: pre-fix the program COMPILED (no diagnostic
// at all) and then SEGFAULTED at run time — the lowering read the length and
// the data pointer out of a null string header. MEASURED pre-fix under ASan:
// "AddressSanitizer: SEGV ... in main". It is now a compile-time rejection.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           reject_null_separator) {
    auto r = runAot(prog(std::string(kParts) +
                         "    Console.WriteLine(string.Join(null, parts));\n" +
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(nullableSeparator(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// A `string?` that §7.4 has NOT narrowed — the same §7 rule as the `null`
// literal, and the boundary the next row sits on.
//
// ⚠ The un-narrowed nullable separator must be a PARAMETER, not a local:
// §7.4 narrows a declared-nullable LOCAL by its initializer ("the
// declaration's assignment"), so `var sep: string? := ","` is already a
// `string` at its use and is ADMISSIBLE (that is the accept-control row
// below, spelled with a narrowing guard). A parameter carries no such fact.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           reject_unnarrowed_nullable_separator) {
    const std::string src =
        "module M;\nclass C\n"
        "  private static method Use(sep: string?): int\n" +
        std::string(kParts) +
        "    Console.WriteLine(string.Join(sep, parts));\n"
        "    return 0;\n"
        "  end Use;\n"
        "  public static method Main(): int\n"
        "    return C.Use(\",\");\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(nullableSeparator(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// ── ACCEPT-CONTROLS — the separators §9.6 DOES declare ────────────────────
// Every control asserts a VALUE, so a control that stopped lowering would
// fail here rather than silently returning something else.

// The declared spelling: a `string` separator.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           accept_string_separator) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(string.Join(\",\", "
                                  "parts));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "ab,cd");
}

// §7.4's narrowing: a `string?` that has been proved non-null HAS type
// `string` at the use site, so it is admissible. This is the row that would
// break if the gate read the DECLARED type instead of the narrowed one.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           accept_narrowed_nullable_separator) {
    const std::string src = prog(std::string(kParts) +
                                  "    var sep: string? := \",\";\n"
                                  "    if sep is null then return 1; end;\n"
                                  "    Console.WriteLine(string.Join(sep, "
                                  "parts));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "ab,cd");
}

// §7.4's OTHER narrowing fact — "the declaration's assignment": a declared-
// nullable LOCAL is narrowed by its initializer, so `var sep: string? := ","`
// is already a `string` at its use and needs no guard. This is the row that
// would break if the gate read the DECLARED type instead of the narrowed
// one.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           accept_init_narrowed_nullable_separator) {
    const std::string src = prog(std::string(kParts) +
                                  "    var sep: string? := \",\";\n"
                                  "    Console.WriteLine(string.Join(sep, "
                                  "parts));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "ab,cd");
}

// §9.6 declares `items: IEnumerable<string>` — NOT just a concrete
// `Array<string>`. The #1182 lazy-source lowering must keep working behind
// the new gate.</newinput>
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           accept_string_separator_over_lazy_query) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  using var xs := new Array<string>(); "
        "  xs.Add(\"ab\"); xs.Add(\"cd\"); "
        "  var s := string.Join(\",\", xs.Where(lambda(x: string) => "
        "x.Length > 0)); "
        "  Console.WriteLine(s); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "ab,cd");
}

// ── THE SIBLING GATES ARE UNCHANGED ───────────────────────────────────────
// A fix that moved or widened the gate must not have relaxed the §9.6 ARITY
// gate (#7140) that keys on the argument COUNT.

// The one-argument form: rejected by the ARITY gate, with ITS wording.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           unchanged_arity_gate_still_rejects_one_argument) {
    auto r = runAot(prog(std::string(kParts) +
                         "    Console.WriteLine(string.Join(parts));\n" +
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityDiagnostic(r));
    BEZEL_EXPECT_FALSE(notConvertible(r));
}

// The three-argument form: likewise.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           unchanged_arity_gate_still_rejects_three_arguments) {
    auto r = runAot(prog(std::string(kParts) +
                         "    Console.WriteLine(string.Join(\",\", parts, "
                         "parts));\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityDiagnostic(r));
    BEZEL_EXPECT_FALSE(notConvertible(r));
}

// `Join` is a STATIC §9.6 member: there is no `int.Join`, and the type
// name carries no such member.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           unchanged_non_string_class_has_no_join) {
    auto r = runAot(prog(std::string(kParts) +
                         "    Console.WriteLine(int.Join(\",\", parts));\n" +
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// The INSTANCE spelling is not a §9.6 member either, and its own diagnostic
// must survive.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           unchanged_instance_spelling_still_rejected) {
    auto r = runAot(prog(
        "    var s: string := \"ab\";\n"
        "    Console.WriteLine(s.Join(\",\"));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// The sibling `string.Repeat` count gate (#7132) is a DIFFERENT gate at a
// different position; a fix here must not have disturbed it.
BEZEL_TEST(sema_string_join_separator_type_gate_7150,
           unchanged_sibling_repeat_count_gate) {
    auto r = runAot(prog(
        "    var b: uint := 4000000000;\n"
        "    Console.WriteLine(string.Repeat('x', b).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("the argument to `string.Repeat` has a type that "
                           "is not implicitly convertible to the target type "
                           "(§5.1)") != std::string::npos)
            return;
    BEZEL_EXPECT_TRUE(false);
}
