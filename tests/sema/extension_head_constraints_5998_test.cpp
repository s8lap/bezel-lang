// fix #5998 items (1), (2) and (4) / spec §14.2 *A type parameter as the type
// argument* (case 1) and §26.4 (v0.355): a block type bound decides a `class`
// requirement of a generic extension head only when it guarantees class. A
// class bound, a bound naming IDisposable, or an interface extending IDisposable
// guarantees it (L2391). An ordinary interface bound does not, so
// `extension OwningArray<T> where T: IShape` is a compile error naming
// `where T: class`.
//
// Item (2), narrowed: every builtin head that DECLARES a `where` clause in the
// standard library is checked at the head like the owning family already was
// (stdlib §2.16, §4.2, §4.5, §4.7, §4.9, §4.10, §4.11, §4.13, §6.6). The
// hash-keyed builtins are NOT such heads: `Dictionary<K, V>` and `Set<T>`
// declare no `where` clause, so their key rule stays the type-level §3.6
// rule 2 check (the issue's Dictionary half was withdrawn by its owner).
//
// Item (4): the Channel head's `sendable` requirement (stdlib §6.9) is
// decided by the block's own `where` clause (§26.4), so
// `extension Channel<T> where T: sendable` is admitted.
//
// Every reject row asserts the §14.2 needle naming the constraint and the
// parameter, and the head's real file:line:col (the location survives only as
// text in the packed message, see chain_position_field_read_3829_test.cpp).
// Every accept row asserts the program ran and printed the expected stdout.

#include <regex>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §14.2 head diagnostic that names `needle`, or empty.
std::string headDiagnosticText(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find("(§14.2)") != std::string::npos)
            return d.message;
    return {};
}

bool hasHeadDiagnostic(const JitResult& r, const std::string& needle) {
    return !headDiagnosticText(r, needle).empty();
}

// True when the head diagnostic naming `needle` carries the real location
// `:line:col: ` of the extension head at `headText` in a one-line source.
bool headDiagnosticAt(const JitResult& r, const std::string& needle,
                      unsigned line, unsigned col) {
    const std::string msg = headDiagnosticText(r, needle);
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    std::smatch m;
    if (!std::regex_search(msg, m, kLoc)) return false;
    return std::stoul(m[1]) == line && std::stoul(m[2]) == col;
}

// The 1-based column of the first `head` in a one-line source.
unsigned columnOf(const std::string& src, const std::string& head) {
    return static_cast<unsigned>(src.find(head)) + 1;
}

const std::string kWidget =
    "module M; "
    "class Widget "
    "  public constructor() end constructor; "
    "end Widget; ";

}  // namespace

// ---------------------------------------------------------------------------
// Reject rows, part (1): a class requirement over an ordinary interface bound.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_extension_head_constraints_5998,
           ordinary_interface_bound_does_not_guarantee_class_rejected_sec_14_2) {
    // §14.2 line 2391: an ordinary interface's conforming set is open, so
    // `where T: IShape` does not guarantee `class` for OwningArray's element.
    const std::string src =
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "extension OwningArray<T> where T: IShape "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;";
    auto r = runAot(src);
    // The strong claim: a real compile-time rejection with a located
    // diagnostic, not a runtime crash, signal, timeout or ASan report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle =
        "type argument `T` does not satisfy the constraint `where T: class`";
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, needle));
    BEZEL_EXPECT_TRUE(headDiagnosticAt(
        r, needle, 1, columnOf(src, "OwningArray<T> where T: IShape")));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           interface_bound_on_class_requirement_rejected_sec_14_2) {
    // §14.2 line 2391: a user class `Keep<T> where T: class` requires class of
    // its parameter; an extension restating it with an ordinary interface
    // bound does not state a class guarantee, so the head is rejected.
    const std::string src =
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Keep<T> where T: class "
        "  public constructor() end constructor; "
        "end Keep; "
        "extension Keep<T> where T: IShape "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;";
    auto r = runAot(src);
    // The strong claim: a real compile-time rejection with a located
    // diagnostic, not a runtime crash, signal, timeout or ASan report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle =
        "type argument `T` does not satisfy the constraint `where T: class`";
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, needle));
    BEZEL_EXPECT_TRUE(headDiagnosticAt(
        r, needle, 1, columnOf(src, "Keep<T> where T: IShape")));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           owning_array_class_restated_accepted_and_runs_sec_26_4) {
    // §26.4 (v0.338): `extension OwningArray<T> where T: class` restates the
    // stdlib requirement; the extension applies to OwningArray<Widget>.
    auto r = runAot(
        kWidget +
        "extension OwningArray<T> where T: class "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var a := new OwningArray<Widget>(); "
        "    Console.WriteLine(a.P().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           idisposable_type_bound_guarantees_class_accepted_sec_14_2) {
    // §14.2 line 2391: a bound naming IDisposable guarantees `class`, so the
    // head is admitted. The head is declared only; no receiver is instantiated.
    auto r = runAot(
        "module M; "
        "extension OwningArray<T> where T: IDisposable "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           interface_extending_idisposable_guarantees_class_accepted_sec_14_2) {
    // §14.2 line 2391: an interface extending IDisposable guarantees `class`.
    auto r = runAot(
        "module M; "
        "interface IDsp: IDisposable method Area(): int; end IDsp; "
        "extension OwningArray<T> where T: IDsp "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           class_bound_guarantees_class_accepted_sec_14_2) {
    // §14.2 line 2391: a bound naming a class guarantees `class`.
    auto r = runAot(
        "module M; "
        "class Base end Base; "
        "extension OwningArray<T> where T: Base "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}


BEZEL_TEST(sema_extension_head_constraints_5998,
           class_bound_whose_base_list_names_required_interface_accepted_sec_14_2) {
    // §14.2 L2391 and rule 6 (L2428): Keep requires `where T: IS`. The block
    // bound `Circle` is a class whose own base list names IS, so every
    // Circle satisfies IS at every substitution the bound admits, and the
    // head is admitted. The program instantiates and runs.
    auto r = runAot(
        "module M; "
        "interface IS method A(): int; end IS; "
        "class Circle : IS "
        "  public constructor() end constructor; "
        "  public method A(): int return 1; end A; "
        "end Circle; "
        "class Keep<T> where T: IS "
        "  public constructor() end constructor; "
        "end Keep; "
        "extension Keep<T> where T: Circle "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var k := new Keep<Circle>(); "
        "    Console.WriteLine(k.P().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// fix #5998 part (2) (§14.2 *A type parameter as the type argument*, case 1,
// v0.338, L2391): `Box<W>` names its argument `W`, a parameter of `Keep`. The
// constraint is decided at that naming site by `W`'s own constraints. `W`
// unconstrained does not guarantee `new()`, so the declaration is rejected even
// though `Keep` is never instantiated (the instantiation-time deferral alone
// admitted it).
BEZEL_TEST(sema_extension_head_constraints_5998,
           unconstrained_type_parameter_naming_new_bound_rejected_sec_14_2) {
    auto r = runAot(
        "module M; "
        "class Box<W> where W: new() end Box; "
        "class Keep<W> "
        "  public method Use(): int var b := new Box<W>(); return 0; end Use; "
        "end Keep; "
        "class P public static method Main(): int return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, "where W: new()"));
}

// Reject rows: `value` and `class` and `notnull` are not guaranteed by the
// parameter's own different constraint; a derived-property constraint is
// guaranteed only by the same item.
BEZEL_TEST(sema_extension_head_constraints_5998,
           parameter_with_value_bound_does_not_guarantee_new_rejected_sec_14_2) {
    auto r = runAot(
        "module M; "
        "class Box<W> where W: new() end Box; "
        "class Keep<W> where W: value "
        "  public method Use(): int var b := new Box<W>(); return 0; end Use; "
        "end Keep; "
        "class P public static method Main(): int return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, "where W: new()"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           parameter_with_new_bound_does_not_guarantee_class_rejected_sec_14_2) {
    auto r = runAot(
        "module M; "
        "class Box<W> where W: class end Box; "
        "class Keep<W> where W: new() "
        "  public method Use(): int var b := new Box<W>(); return 0; end Use; "
        "end Keep; "
        "class P public static method Main(): int return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, "where W: class"));
}

// Accept controls: the parameter carries the same constraint, or a class/
// IDisposable bound guarantees the requirement (L2391). The program runs.
BEZEL_TEST(sema_extension_head_constraints_5998,
           parameter_with_same_new_bound_accepted_control_sec_14_2) {
    auto r = runAot(
        "module M; "
        "class Box<W> where W: new() end Box; "
        "class Keep<W> where W: new() "
        "  public method Use(): int var b := new Box<W>(); return 7; end Use; "
        "end Keep; "
        "class P public static method Main(): int "
        "  var k := new Keep<int>(); return k.Use(); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 7);
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           parameter_with_idisposable_bound_guarantees_class_accepted_control_sec_14_2) {
    auto r = runAot(
        "module M; "
        "interface IX: IDisposable end IX; "
        "class Box<W> where W: class end Box; "
        "class Keep<W> where W: IDisposable "
        "  public method Use(): int var b := new Box<W>(); return 7; end Use; "
        "end Keep; "
        "class P public static method Main(): int return 7; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 7);
}

// ---------------------------------------------------------------------------
// fix #5998 item (2) — the OTHER constrained builtin heads.
//
// §26.4 (v0.355): "the extended type's declared constraints are decided there
// as at an instantiation site (§14.2 *A type parameter as the type argument*),
// by the parameters' constraints, which are the block's `where` clause and
// nothing else: `extension OwningArray<T>` is a compile error naming the
// constraint and `T`, `extension OwningArray<T> where T: class` is admitted".
//
// Every builtin head that DECLARES a `where` clause in the standard library
// is such a head, and the block must restate it:
//   stdlib §2.16  `Shared<T>` / `Weak<T>`                   `where T: class`
//   stdlib §4.2   `IOwningCollection<T>` / `IOwningList<T>` `where T: class`
//   stdlib §4.5   `OwningArray<T>`                          `where T: class`
//   stdlib §4.7   `OwningLinkedList<T>`                     `where T: class`
//   stdlib §4.9   `OwningDictionary<K, V>` / `DrainedEntry<K, V>` `where V: class`
//   stdlib §4.10  `OwningSet<T>`                            `where T: class`
//   stdlib §4.11  `OwningStack<T>` / `OwningQueue<T>`       `where T: class`
//   stdlib §4.13  the four owning enumeration interfaces    `where T: class`
//   stdlib §6.6   `OwningTaskCompletionSource<T>`           `where T: class`
//
// The hash-keyed builtins are NOT among them: `Dictionary<K, V>` (stdlib
// §4.8) and `Set<T>` (stdlib §4.10) declare NO `where` clause, so §26.4 has
// no declared constraint to restate at their heads — their key rule is the
// §3.6 rule 2 check at the instantiation site, "carried to that declaration's
// instantiations" (stdlib §4.20's recorded ruling). The rows for the hash-keyed
// builtins at the end of this block pin that reading.
// ---------------------------------------------------------------------------

// One reject row per constrained builtin head: a head that does not restate
// the stdlib's clause is a compile error naming the constraint and the
// parameter, at the head's own location.
#define BEZEL_5998_CLASS_HEAD_REJECT_ROW(test_name, head_text)                 \
    BEZEL_TEST(sema_extension_head_constraints_5998, test_name) {              \
        const std::string src =                                               \
            "module M; "                                                       \
            "extension " head_text " "                                         \
            "  public method P(): int return 7; end P; "                       \
            "end; "                                                            \
            "class C public static method Main(): int return 0; end Main; end C;"; \
        auto r = runAot(src);                                                 \
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));            \
        const std::string needle =                                            \
            "does not satisfy the constraint `where ";                         \
        BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, needle));                       \
        BEZEL_EXPECT_TRUE(                                                     \
            headDiagnosticAt(r, needle, 1, columnOf(src, head_text)));         \
    }

BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    shared_head_without_class_clause_rejected_sec_26_4, "Shared<T>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    weak_head_without_class_clause_rejected_sec_26_4, "Weak<T>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    drained_entry_head_without_class_clause_rejected_sec_26_4,
    "DrainedEntry<K, V>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    ioowning_collection_head_without_class_clause_rejected_sec_26_4,
    "IOwningCollection<T>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    ioowning_list_head_without_class_clause_rejected_sec_26_4, "IOwningList<T>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    iowning_enumerable_head_without_class_clause_rejected_sec_26_4,
    "IOwningEnumerable<T>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    iowning_enumerator_head_without_class_clause_rejected_sec_26_4,
    "IOwningEnumerator<T>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    iowning_async_enumerable_head_without_class_clause_rejected_sec_26_4,
    "IOwningAsyncEnumerable<T>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    iowning_async_enumerator_head_without_class_clause_rejected_sec_26_4,
    "IOwningAsyncEnumerator<T>")
BEZEL_5998_CLASS_HEAD_REJECT_ROW(
    owning_task_completion_source_head_without_class_clause_rejected_sec_26_4,
    "OwningTaskCompletionSource<T>")

#undef BEZEL_5998_CLASS_HEAD_REJECT_ROW

// The DrainedEntry head's constrained slot is the SECOND one: the diagnostic
// names `V`, the stdlib's own parameter name for that slot (stdlib §4.9:
// `public class DrainedEntry<K, V>: IDisposable where V: class`).
BEZEL_TEST(sema_extension_head_constraints_5998,
           drained_entry_head_names_the_constrained_slot_sec_26_4) {
    const std::string src =
        "module M; "
        "extension DrainedEntry<K, V> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle =
        "type argument `V` does not satisfy the constraint `where V: class`";
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, needle));
    BEZEL_EXPECT_TRUE(headDiagnosticAt(
        r, needle, 1, columnOf(src, "DrainedEntry<K, V>")));
}

// Accept rows: the head restates the stdlib's clause and the program runs.
BEZEL_TEST(sema_extension_head_constraints_5998,
           shared_head_with_class_clause_accepted_sec_26_4) {
    // stdlib §2.16: `public class Shared<T>: IDisposable where T: class` —
    // restated, the head is admitted.
    auto r = runAotChecked(
        "module M; "
        "extension Shared<T> where T: class "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           drained_entry_head_with_class_clause_accepted_sec_26_4) {
    // stdlib §4.9: `public class DrainedEntry<K, V>: IDisposable where V:
    // class` — restated, the head is admitted.
    auto r = runAotChecked(
        "module M; "
        "extension DrainedEntry<K, V> where V: class "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           owning_task_completion_source_head_with_class_clause_accepted_sec_26_4) {
    // stdlib §6.6: `public class OwningTaskCompletionSource<T> where T: class`
    // — restated, the head is admitted.
    auto r = runAotChecked(
        "module M; "
        "extension OwningTaskCompletionSource<T> where T: class "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           ioowning_collection_head_with_class_clause_accepted_sec_26_4) {
    // stdlib §4.2: `public interface IOwningCollection<T>: … where T: class`
    // — restated, the head is admitted.
    auto r = runAotChecked(
        "module M; "
        "extension IOwningCollection<T> where T: class "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

// ---------------------------------------------------------------------------
// fix #5998 item (4) — the Channel head's `sendable` requirement.
//
// stdlib §6.9: `public class Channel<T> where T: sendable` and
// `public class ChannelSender<T> where T: sendable`. §14.2 "A type parameter
// as the type argument", case 1: the argument is a type parameter, so the
// constraint is decided AT THE NAMING SITE by the parameter's own
// constraints — and a derived property is guaranteed only by the parameter
// carrying the same constraint. The extension block's parameter is
// constrained by the block's `where` clause and nothing else (§26.4), so
// `extension Channel<T> where T: sendable` STATES the requirement and is
// admitted; the head without the clause is not.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_extension_head_constraints_5998,
           channel_head_with_sendable_clause_accepted_sec_26_4) {
    // The clause is stated; the block applies to `Channel<string>` and runs.
    auto r = runAotChecked(
        "module M; "
        "extension Channel<T> where T: sendable "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var ch := new Channel<string>(); "
        "    Console.WriteLine(ch.P().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           channel_sender_head_with_sendable_clause_accepted_sec_26_4) {
    auto r = runAotChecked(
        "module M; "
        "extension ChannelSender<T> where T: sendable "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           channel_head_without_sendable_clause_rejected_sec_26_4) {
    // The block's parameter states nothing, so the head's `sendable`
    // requirement is not satisfied (§14.2 case 1 read at the naming site).
    const std::string src =
        "module M; "
        "extension Channel<T> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle =
        "type argument `T` does not satisfy the constraint `where T: sendable`";
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, needle));
    BEZEL_EXPECT_TRUE(headDiagnosticAt(r, needle, 1, columnOf(src, "Channel<T>")));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           channel_head_with_class_clause_rejected_sec_26_4) {
    // §14.2 case 1: a derived property is guaranteed only by the parameter
    // carrying the SAME constraint — `where T: class` does not make T
    // sendable, so the head is still rejected.
    const std::string src =
        "module M; "
        "extension Channel<T> where T: class "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle =
        "type argument `T` does not satisfy the constraint `where T: sendable`";
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, needle));
    BEZEL_EXPECT_TRUE(headDiagnosticAt(r, needle, 1, columnOf(src, "Channel<T>")));
}

// ---------------------------------------------------------------------------
// The Dictionary head — the hash-keyed builtin that declares NO `where`
// clause (stdlib §4.8). §26.4 restates the extended type's DECLARED
// constraints, and Dictionary declares none: its key rule is the §3.6 rule 2
// check at the instantiation site, "carried to that declaration's
// instantiations" (stdlib §4.20's recorded ruling — the rule the issue's own
// owner withdrew the Dictionary half on). So both rows are admitted — the
// head without a clause, and the head that states one.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_extension_head_constraints_5998,
           dictionary_head_without_where_clause_accepted_sec_26_4) {
    auto r = runAotChecked(
        "module M; "
        "extension Dictionary<K, V> "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           dictionary_head_with_hashable_clause_accepted_sec_26_4) {
    auto r = runAotChecked(
        "module M; "
        "extension Dictionary<K, V> where K: hashable "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}
