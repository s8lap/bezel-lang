// Regression tests for issue #7490 — a NESTED `record` ESCAPED EVERY §9.6/§9.7
// RECORD RESTRICTION.
//
// THE GOVERNING TEXT.
//
//   spec §28.12 (v0.350) — "`IAsyncDisposable` extends `IDisposable`, so it
//   conforms to `IDisposable` under §14.2's rule 3 … and only a class implements
//   it — a record, enum or choice that lists it, or lists an interface
//   extending it, is a compile error at the declaration, as no value kind
//   implements `IDisposable`."
//
//   spec §9.7 (v0.350) — "Records cannot have destructors, `owned` fields, or
//   implement `IDisposable` — directly or through an interface extending it,
//   `IAsyncDisposable` among them."
//
//   spec §8.7 (v0.350) — "`Dispose()` is class-only. The compiler generates
//   `IDisposable` on every non-static class. Static classes, records, enums,
//   choices, primitives, and inline arrays do not implement `IDisposable`."
//
//   spec §9.6 (v0.350) — "Records are implicitly sealed" and "A record cannot
//   derive from another record or from a class."
//
//   spec §11.6 (v0.350) — "Nested types may themselves contain nested types",
//   so a nested type is an ordinary declaration of its kind at ANY depth, with
//   no exemption from §9.6 / §9.7 / §28.12.
//
// THE DEFECT.
//
//   `Resolver::checkRecordRestrictions` iterated `ctx_.module()->children` —
//   MODULE-LEVEL ONLY. So a `record` declared INSIDE A CLASS BODY was never
//   visited by ANY of §9.7's checks: it could name `IDisposable` (or an
//   interface extending it), derive from a class, declare a destructor or an
//   `owned` field, be `abstract`, declare an `event`, or contain itself by
//   value — each ADMITTED.
//
//   THE ASYMMETRY IS THE DEFECT. §28.12 states ONE sentence, and the enum
//   and choice arms of it were applied at every nesting depth (both
//   `checkEnumRestrictions` and `checkChoiceRestrictions` route through the
//   shared recursive walker `forEachTypeAndNested`, fix #3574) while the
//   record arm was applied at depth 0 only. §28.12 is not applied at "three
//   different depths" anywhere in the spec.
//
//   ONE OF THE ESCAPED SHAPES WAS A COMPILER CRASH: a nested record that
//   contains itself by value (`public _next: R? := null;`) reached codegen
//   with an unbounded record-struct layout recursion and died on SIGILL. The
//   module-level twin of the same program is rejected with the §9
//   self-containment diagnostic. The crash is the same reach gap, one level
//   worse.
//
// THE FIX.
//
//   A REACH CHANGE ONLY: `checkRecordRestrictions` now walks the module's own
//   declaration tree with the SAME recursive walker its sibling checks use
//   (`forEachTypeAndNested`, fix #3574, §11.6). The predicates, the
//   diagnostic texts, and the diagnostic sites are untouched.
//
// THE ORACLE.
//
//   A rejection is pinned by the DIAGNOSTIC'S TEXT at a REAL LOCATION — never
//   by "the program did not compile" and never by an exit code.
//
// THE CONTROLS.
//
//   A nested record naming NO disposal interface is still ADMITTED; a nested
//   CLASS implementing `IDisposable` is still ADMITTED (§28.12: "only a class
//   implements it"); a nested class with a class base is still ADMITTED; a
//   nested INTERFACE extending a disposal interface is still ADMITTED (§28.12
//   names record/enum/choice only); the module-level record verdicts are
//   unchanged; and a nested ENUM / CHOICE naming a disposal interface is
//   still REFUSED, as it was before this fix.
//
// Cite: language spec §28.12 / §9.7 / §9.6 / §8.7 / §11.6 / §14.2 rule 3
// (v0.350).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

#include <string>
#include <vector>

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Builds a program from `lines`; the 1-based line number of `lines[i]` is
// i + 1, so a row can name the line it expects the diagnostic on without
// counting newlines by hand.
std::string prog(const std::vector<std::string>& lines) {
    std::string s;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        s += "\n";
    }
    return s;
}

// The oracle: the program was REJECTED AT COMPILE TIME, and the diagnostic
// carrying the expected TEXT sits at the expected REAL LOCATION.
bool rejectedAt(const std::string& program, int line, int col,
                const char* text) {
    JitResult r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    const std::string needle =
        ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        std::size_t pos = 0;
        while ((pos = d.message.find(needle, pos)) != std::string::npos) {
            const std::size_t start = pos + needle.size();
            const std::size_t eol = d.message.find('\n', start);
            const std::string one =
                d.message.substr(start, eol == std::string::npos
                                          ? std::string::npos
                                          : eol - start);
            if (one.find(text) != std::string::npos) return true;
            pos += needle.size();
        }
    }
    return false;
}

// The control oracle for a program that must COMPILE (it need not run).
void expectCompiles(const std::string& program) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A `Program` with a `Main`, appended so an admitted program links and runs.
const char* kMain = "public class Program";
const char* kMainBody = "  public static method Main()";
const char* kMainPrint = "    Console.WriteLine(\"hi\");";
const char* kMainEnd = "  end Main;";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// REJECT rows — the nested record is now refused, at a REAL LOCATION.
// ═══════════════════════════════════════════════════════════════════════════

// THE SHAPE THE ISSUE NAMES. §9.7: "Records cannot have … implement
// `IDisposable`". MEASURED on the pre-fix tip: this COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_listing_idisposable_is_rejected_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public record R : IDisposable",
        "    public _x: int := 0;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedAt(p, 3, 10, "record `R` may not implement `IDisposable` (§9.7)"));
}

// §28.12's chain, one link down, at nested depth. MEASURED on the pre-fix
// tip: this COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_listing_i_async_disposable_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public record R : IAsyncDisposable",
        "    public _x: int := 0;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 3, 10,
        "record `R` may not implement `IAsyncDisposable` — "
        "`IAsyncDisposable` extends `IDisposable` (§9.7, §28.12)"));
}

// §28.12's chain at nested depth, through a USER-DEFINED interface — the row
// a NAME-based check cannot reach, and the one the §9.7 text names
// explicitly ("directly or through an interface extending it").
// MEASURED on the pre-fix tip: this COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_listing_a_user_interface_extending_idisposable_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public interface MyDisp: IDisposable end MyDisp;",
        "public class Outer",
        "  public record R : MyDisp",
        "    public _x: int := 0;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 10,
        "record `R` may not implement `MyDisp` — `MyDisp` extends "
        "`IDisposable` (§9.7, §28.12)"));
}

// §9.6's CLASS-BASE BAN, at nested depth: "A record cannot derive from
// another record or from a class." MEASURED on the pre-fix tip: this
// COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_deriving_from_a_class_is_rejected_spec_9_6) {
    const std::string p = prog({
        "module Conf;",
        "public class Base end Base;",
        "public class Outer",
        "  public record R : Base",
        "    public _x: int := 0;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 10,
        "record `R` cannot derive from class `Base` (§9.6); records may only "
        "implement interfaces"));
}

// §9.7's DESTRUCTOR BAN, at nested depth. MEASURED on the pre-fix tip: this
// COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_with_a_destructor_is_rejected_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public record R",
        "    destructor",
        "    end destructor;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedAt(p, 4, 5, "record `R` may not declare a destructor (§9.7)"));
}

// §9.7's `owned`-FIELD BAN, at nested depth. MEASURED on the pre-fix tip:
// this COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_with_an_owned_field_is_rejected_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public class Res",
        "  public _x: int := 0;",
        "end Res;",
        "public class Outer",
        "  public record R",
        "    public owned _r: Res := new Res();",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 7, 5, "record `R` may not have an `owned` field `_r` (§9.7)"));
}

// §9.6's SEALEDNESS, at nested depth: a record is implicitly sealed and can
// never be derived from, so an `abstract` record could never have a concrete
// subclass to provide bodies for its abstract members. MEASURED on the
// pre-fix tip: this COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_abstract_record_is_rejected_spec_9_6) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public abstract record R",
        "    public _x: int := 0;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 3, 19,
        "record `R` may not be `abstract` — records are implicitly sealed"));
}

// §9's SELF-CONTAINMENT rule, at nested depth — the shape that CRASHED the
// compiler on the pre-fix tip (SIGILL in codegen: an unbounded record-struct
// layout recursion). The module-level twin of the same program is rejected
// with this diagnostic.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_containing_itself_by_value_is_rejected_spec_9) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public record R",
        "    public _next: R? := null;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 3, 10,
        "record `R` cannot recursively contain itself by value — a record is "
        "stored inline"));
}

// §24 / §9.7's EVENT ban, at nested depth: an event's compiler-generated
// `owned EventSource` backing field requires an owning, disposable
// container, which a record is not. MEASURED on the pre-fix tip: this
// COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_with_an_event_member_is_rejected_spec_24) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public record R",
        "    public event Changed: Object;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 5,
        "record `R` may not declare an `event` member — an event's "
        "compiler-generated `owned EventSource` backing field requires an "
        "owning, disposable container, which a record is not (§24, §9.7)"));
}

// THE DEPTH ROW: a record nested TWO levels deep (a class inside a class).
// §11.6: "Nested types may themselves contain nested types" — so the reach
// must be RECURSIVE, not one level deep. MEASURED on the pre-fix tip: this
// COMPILED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_at_depth_two_listing_idisposable_is_rejected_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public class A",
        "  public class B",
        "    public record R : IDisposable",
        "      public _x: int := 0;",
        "    end R;",
        "  end B;",
        "end A;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedAt(p, 4, 12, "record `R` may not implement `IDisposable` (§9.7)"));
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROL rows — the fix must not change any of these verdicts.
// ═══════════════════════════════════════════════════════════════════════════

// ADMITTING TWIN 1 — a nested record naming NO disposal interface is still
// ADMITTED. The check keys on the disposal CHAIN, not on "the record is
// nested" or "the record lists an interface".
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_listing_a_non_disposal_interface_is_still_admitted_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public interface IFoo method Bar(): int; end IFoo;",
        "public class Outer",
        "  public record R : IFoo",
        "    public method Bar(): int => 7;",
        "  end R;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// ADMITTING TWIN 1b — a nested record with NO base list at all is still
// ADMITTED.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_record_without_any_base_list_is_still_admitted_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public record R",
        "    public _x: int := 0;",
        "  end R;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// ADMITTING TWIN 2 — §28.12: "only a class implements it". A nested CLASS
// implementing `IDisposable` is still ADMITTED, and a nested class deriving
// from a class is still ADMITTED (§11.1's single inheritance).
BEZEL_TEST(nested_record_restrictions_7490,
           nested_class_listing_idisposable_is_still_admitted_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public class Inner : IDisposable",
        "  end Inner;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// The nested class's CLASS-BASE half: §9.6's ban is on RECORDS deriving from
// a class, not on classes.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_class_deriving_from_a_class_is_still_admitted_spec_11_1) {
    const std::string p = prog({
        "module Conf;",
        "public class Base end Base;",
        "public class Outer",
        "  public class Inner : Base",
        "  end Inner;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// ADMITTING TWIN 3 — a nested INTERFACE extending a disposal interface is
// still ADMITTED: §28.12's declaration-site ban names "a record, enum or
// choice", and an interface is not one of them.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_interface_extending_a_disposal_interface_is_still_admitted_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public interface MyDisp: IDisposable end MyDisp;",
        "public class Outer",
        "  public interface INested : MyDisp",
        "  end INested;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// ADMITTING TWIN 4 — the MODULE-LEVEL record is unchanged: still REFUSED,
// with the SAME wording and the SAME declaration site.
BEZEL_TEST(nested_record_restrictions_7490,
           module_level_record_listing_idisposable_is_still_rejected_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public record R: IDisposable public _x: int := 0; end R;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedAt(p, 2, 8, "record `R` may not implement `IDisposable` (§9.7)"));
}

// The module-level record's OTHER §9.7 ban — unchanged.
BEZEL_TEST(nested_record_restrictions_7490,
           module_level_record_with_a_destructor_is_still_rejected_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public record R",
        "  destructor",
        "  end destructor;",
        "end R;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedAt(p, 3, 3, "record `R` may not declare a destructor (§9.7)"));
}

// ADMITTING TWIN 5 — a nested ENUM naming a disposal interface is still
// REFUSED, as it was BEFORE this fix (the enum arm of §28.12 already
// reached every depth).
BEZEL_TEST(nested_record_restrictions_7490,
           nested_enum_listing_idisposable_is_still_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public enum E: IDisposable A, B; end E;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 3, 10,
        "enum `E` may not implement `IDisposable` (§8.7, §28.12)"));
}

// The nested CHOICE half of the same control.
BEZEL_TEST(nested_record_restrictions_7490,
           nested_choice_listing_idisposable_is_still_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public choice C: IDisposable A, B; end C;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 3, 10,
        "choice `C` may not implement `IDisposable` (§8.7, §28.12)"));
}

// The module-level record's ADMITTING twin: a module-level record naming a
// NON-disposal interface is still ADMITTED (unchanged).
BEZEL_TEST(nested_record_restrictions_7490,
           module_level_record_listing_a_non_disposal_interface_is_still_admitted_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public interface IFoo method Bar(): int; end IFoo;",
        "public record R: IFoo",
        "  public method Bar(): int => 7;",
        "end R;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}
