// Regression tests for issue #9250 — an assignment to the READ-ONLY `Length`
// of a `string`, a `StringBuilder` or an `OwningBuffer<T>` COMPILED with only
// an `internal:` warning, and the store was silently dropped.
//
// THE DEFECT. The stdlib declares `Length` on four heads the compiler knows,
// and every one of them is a GET-ONLY property:
//
//   stdlib §3.1  (v0.359)  StringBuilder — `public readonly property Length: int;`
//   stdlib §4.14 (v0.359)  OwningBuffer<T> — `public pure readonly property Length: int;`
//   stdlib §4.19 (v0.359)  FrozenArray<T> — `public readonly property Length: int;`
//   stdlib §9.6  (v0.359)  String — `public pure readonly property Length: int;`
//
// Language spec §23.3 (v0.359) makes a property with no setter read-only
// ("A read-only property in block form omits `set`"), and §23.1 (v0.359) says
// a `readonly` auto-property's assignment "is a store to the backing field
// (no setter exists to invoke, §22.7 rule 1)" and is admitted "only in a
// field initializer or in a constructor" of its declaring type — which a
// compiler-intrinsic `StringBuilder` / `OwningBuffer<T>` / `FrozenArray<T>` /
// `string` never has. §17.6 (v0.359) adds "Read-only locations cannot be
// incremented." So every one of these writes is a compile error.
//
// MEASURED on the base r30-c1 (`cb9f7153e`), on the issue's own programs:
//
//     t.Length := 3;    (a `string` local)
//       -> COMPILED (exit 0) with exactly one diagnostic:
//          "internal: assignment target lvalue address failed to resolve
//           (store dropped — target retains its previous value) — #4045"
//
// i.e. a SILENT ACCEPTANCE: the store was DROPPED and the program ran. The
// same held for `sb.Length := 3;` (a `StringBuilder`), `b.Length := 3;` (an
// `OwningBuffer<byte>`) and `b.S.Length := 3;` (through a `string` field).
// The increment spelling `t.Length++;` produced NO diagnostic at all.
//
// THE CAUSE. These members are BUILTIN: they resolve to no `PropertySymbol`
// (the builtin `Length` heads are typed by
// `common::isBuiltinIntLengthPropertyHead`, not by a property symbol), so
// `ExprTC::checkPropertyWriteTarget` — where every write-target rule lives —
// found no symbol and skipped every rule. The write then reached codegen,
// where a builtin member has no lvalue address to store into, and the store
// was dropped.
//
// THE FIX. The same function now rejects a write to a BUILTIN read-only
// `Length` with the compiler's ORDINARY read-only-property diagnostic —
// byte-identical in shape to the one a user get-only property already gets:
//
//     cannot assign to read-only property `Length` — it has no setter (§23.3)
//
// and, for the increment spelling, §17.6's wording:
//
//     cannot increment/decrement read-only property `Length` — it has no
//     setter (§17.6 / §23.3)
//
// The receiver predicate is the REGISTRY's: `common::isBuiltinIntLengthPropertyHead`
// is the single authority for "which built-in heads declare an int `Length`
// property" — it names §3.1 StringBuilder, §4.14 OwningBuffer<T> and §4.19
// FrozenArray<T> — so the three CLASS heads are one registry read, not three
// hand-spelled names that could drift from the stdlib. The `string` head is
// the one that is not a class: it is the §9.6 primitive, so its receiver
// TypeRef is `Kind::Primitive` with `primitive == PrimitiveType::String`.
//
// Every rejection below asserts on diagnostic TEXT, never on the coarse
// "did not run" signal: runAot's WIFSIGNALED branch synthesizes an Error for
// a runtime SIGSEGV too, so a bare `!ran` cannot tell a genuine rejection
// from a crash. Each row therefore pairs `failedAtCompileTimeWithRealLocation`
// (the house discriminator for "the driver rejected the program at compile
// time, with a real source location, and not a crash / timeout / signal") with
// an assertion on the SPECIFIC diagnostic, and that the diagnostic is an
// ERROR (not the `internal:` #4045 warning the base produced).
//
// Spec: stdlib §9.6 / §3.1 / §4.14 (the four `Length` declarations — the
// `readonly` ones), language spec §23.3 (the read-only-property rule), §23.1
// (a `readonly` auto-property's assignable context), §17.6 ("Read-only
// locations cannot be incremented"), §6.6 (the compound-assignment family),
// §2.3 / §2.7 rule 1 (the shadowing of an always-available name). Spec
// revision v0.359.
//
// The `FrozenArray<T>` head is NOT a row here: #9214's own file
// (`tests/ownership/frozen_array_length_readonly_property_9214_test.cpp`)
// already pins it, and this fix leaves those five rows green.
//
// Model: none — the spec decides the case (stdlib §9.6 / §3.1 / §4.14 +
// §23.3).

#include <initializer_list>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The stdlib §9.6 `string` local (the issue's own program).
const char* kStringLocal =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var t := \"abc\";\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same `string` reached through a FIELD (`b.S.Length := 3;`).
const char* kStringField =
    "module P; import System;\n"
    "public class Box\n"
    "  public S: string;\n"
    "  public constructor(s: string) this.S := s; end constructor;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var b := new Box(\"abc\");\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same `string` reached through a PROPERTY RESULT — a get-only property
// whose value is a `string` (`b.S.Length := 3;`).
const char* kStringPropertyResult =
    "module P; import System;\n"
    "public class Box\n"
    "  private _s: string := \"abc\";\n"
    "  public property S: string\n"
    "    get => _s;\n"
    "  end S;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var b := new Box();\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// stdlib §3.1's `StringBuilder` (the issue's own program).
const char* kStringBuilder =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var sb := new StringBuilder(); sb.Append(\"hey\");\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same head reached through its ANNOTATION shape
// (`var sb: StringBuilder := new StringBuilder();`).
const char* kStringBuilderAnnotated =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var sb: StringBuilder := new StringBuilder();\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// stdlib §4.14's `OwningBuffer<T>` (the issue's own program).
const char* kOwningBuffer =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    using var b := new OwningBuffer<byte>(4);\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same head reached through its ANNOTATION shape.
const char* kOwningBufferAnnotated =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    using var b: OwningBuffer<byte> := new OwningBuffer<byte>(4);\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

std::string with(const char* tmpl, const char* stmt) {
    std::string out(tmpl);
    const std::string needle = "%s";
    const size_t at = out.find(needle);
    if (at != std::string::npos) out.replace(at, needle.size(), stmt);
    return out;
}

// The rejection every row below asserts: the driver rejected the program at
// compile time (a real source location, not a crash / timeout / signal), and
// the diagnostic is the SPECIFIC read-only-property one.
bool rejectedWith(const char* tmpl, const char* stmt,
                  std::initializer_list<const char*> needles) {
    auto r = runAot(with(tmpl, stmt));
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* n : needles)
            if (d.message.find(n) == std::string::npos) { all = false; break; }
        if (all) return true;
    }
    return false;
}

// The needles of the ASSIGNMENT spelling of the rejection.
const std::initializer_list<const char*> kAssignNeedles = {
    "assign to", "read-only property `Length`", "no setter", "23.3"};

// The needles of the INCREMENT spelling (§17.6's wording).
const std::initializer_list<const char*> kIncrementNeedles = {
    "increment/decrement", "read-only property `Length`", "no setter", "17.6",
    "23.3"};

}  // namespace

// stdlib §9.6 + §23.3: the `string` local's `Length` is get-only, so the
// write is rejected with the ordinary read-only-property diagnostic.
BEZEL_TEST(builtin_readonly_length_property_9250,
           string_local_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringLocal, "t.Length := 3;", kAssignNeedles));
}

// §6.6: the compound family (`x op= e` is `x := x op e`) reaches the same
// write-target check and takes the same rejection.
BEZEL_TEST(builtin_readonly_length_property_9250,
           string_local_length_compound_assignment_rejected_spec_6_6) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringLocal, "t.Length += 1;", kAssignNeedles));
}

// §17.6: "The operand must be an assignable lvalue … Read-only locations
// cannot be incremented." The increment spelling of the same write reaches
// the same check (the `incOp` argument of the shared check) and is rejected
// with the increment wording.
BEZEL_TEST(builtin_readonly_length_property_9250,
           string_local_length_increment_rejected_spec_17_6) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringLocal, "t.Length++;", kIncrementNeedles));
}

// §17.6: the decrement spelling is the same rule (the shared check carries
// the operator token, and both spellings take the same wording).
BEZEL_TEST(builtin_readonly_length_property_9250,
           string_local_length_decrement_rejected_spec_17_6) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringLocal, "t.Length--;", kIncrementNeedles));
}

// stdlib §9.6 + §23.3: the same write through a FIELD's `string`. The
// receiver is one hop further out, but the property is the same get-only one.
BEZEL_TEST(builtin_readonly_length_property_9250,
           string_field_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringField, "b.S.Length := 3;", kAssignNeedles));
}

// stdlib §9.6 + §23.3: the same write through a PROPERTY RESULT — the
// receiver is the VALUE of a get-only property, whose type is `string`.
BEZEL_TEST(builtin_readonly_length_property_9250,
           string_property_result_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringPropertyResult, "b.S.Length := 3;",
                     kAssignNeedles));
}

// stdlib §3.1 + §23.3: `StringBuilder.Length` is get-only, so the write is
// rejected with the ordinary read-only-property diagnostic.
BEZEL_TEST(builtin_readonly_length_property_9250,
           stringbuilder_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringBuilder, "sb.Length := 3;", kAssignNeedles));
}

// stdlib §3.1 + §23.3: the same head through its ANNOTATION shape — a
// receiver TypeRef with no TypeSymbol at all.
BEZEL_TEST(builtin_readonly_length_property_9250,
           stringbuilder_annotated_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringBuilderAnnotated, "sb.Length := 3;",
                     kAssignNeedles));
}

// §6.6: the compound family on `StringBuilder.Length`.
BEZEL_TEST(builtin_readonly_length_property_9250,
           stringbuilder_length_compound_assignment_rejected_spec_6_6) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringBuilder, "sb.Length += 1;", kAssignNeedles));
}

// §17.6: the increment spelling on `StringBuilder.Length`.
BEZEL_TEST(builtin_readonly_length_property_9250,
           stringbuilder_length_increment_rejected_spec_17_6) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kStringBuilder, "sb.Length++;", kIncrementNeedles));
}

// stdlib §4.14 + §23.3: `OwningBuffer<T>.Length` is get-only, so the write
// is rejected with the ordinary read-only-property diagnostic.
BEZEL_TEST(builtin_readonly_length_property_9250,
           owningbuffer_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kOwningBuffer, "b.Length := 3;", kAssignNeedles));
}

// stdlib §4.14 + §23.3: the same head through its ANNOTATION shape.
BEZEL_TEST(builtin_readonly_length_property_9250,
           owningbuffer_annotated_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kOwningBufferAnnotated, "b.Length := 3;",
                     kAssignNeedles));
}

// §6.6: the compound family on `OwningBuffer<T>.Length`.
BEZEL_TEST(builtin_readonly_length_property_9250,
           owningbuffer_length_compound_assignment_rejected_spec_6_6) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kOwningBuffer, "b.Length += 1;", kAssignNeedles));
}

// §17.6: the increment spelling on `OwningBuffer<T>.Length`.
BEZEL_TEST(builtin_readonly_length_property_9250,
           owningbuffer_length_increment_rejected_spec_17_6) {
    BEZEL_EXPECT_TRUE(
        rejectedWith(kOwningBuffer, "b.Length++;", kIncrementNeedles));
}

// §9.6 / §3.1 / §4.14: the READ of a builtin read-only `Length` stays
// admitted — it is a property READ, not a write target, and §23.3 governs
// only the write. This row is a boundary control: it is red under the
// plausible wrong fix that rejects every `Length` member access instead of
// every `Length` WRITE.
BEZEL_TEST(builtin_readonly_length_property_9250,
           builtin_readonly_length_reads_stay_admitted_spec_9_6_3_1_4_14) {
    auto r = runAot(
        "module P; import System;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var t := \"abc\";\n"
        "    var sb := new StringBuilder(); sb.Append(\"hey\");\n"
        "    using var b := new OwningBuffer<byte>(4);\n"
        "    Console.WriteLine(t.Length);\n"
        "    Console.WriteLine(sb.Length);\n"
        "    Console.WriteLine(b.Length);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n3\n4\n"));
}

// §2.3 / §2.7 rule 1 + §23.3: a USER type's own WRITABLE `Length` stays
// admitted — the always-available names are shadowable, so a user
// `class StringBuilder` / `class OwningBuffer` wins the name and its own
// members answer. This row is a boundary control: it is red under the
// plausible wrong fix that keys on the head NAME without the
// `isUserDeclaredTypeSym` (§2.3 / #5272) discriminator.
BEZEL_TEST(builtin_readonly_length_property_9250,
           user_writable_length_stays_admitted_spec_2_3) {
    auto r = runAot(
        "module P; import System;\n"
        "public class Box\n"
        "  private _n: int := 0;\n"
        "  public property Length: int\n"
        "    get => _n;\n"
        "    set => _n := value;\n"
        "  end Length;\n"
        "end Box;\n"
        "public class StringBuilder\n"
        "  private _n: int := 0;\n"
        "  public property Length: int\n"
        "    get => _n;\n"
        "    set => _n := value;\n"
        "  end Length;\n"
        "end StringBuilder;\n"
        "public class OwningBuffer\n"
        "  private _n: int := 0;\n"
        "  public property Length: int\n"
        "    get => _n;\n"
        "    set => _n := value;\n"
        "  end Length;\n"
        "end OwningBuffer;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var b := new Box();\n"
        "    b.Length := 3;\n"
        "    b.Length++;\n"
        "    var sb := new StringBuilder();\n"
        "    sb.Length := 5;\n"
        "    sb.Length++;\n"
        "    var ob := new OwningBuffer();\n"
        "    ob.Length := 7;\n"
        "    ob.Length++;\n"
        "    Console.WriteLine(b.Length);\n"
        "    Console.WriteLine(sb.Length);\n"
        "    Console.WriteLine(ob.Length);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n6\n8\n"));
}
