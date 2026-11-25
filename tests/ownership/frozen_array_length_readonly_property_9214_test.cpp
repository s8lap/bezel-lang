// Regression tests for issue #9214 — an assignment to a `FrozenArray<T>`'s
// read-only `Length` COMPILED with only an `internal:` warning, and the store
// was silently dropped.
//
// THE DEFECT. stdlib §4.19 (v0.359) declares
//
//     public readonly property Length: int;
//
// on `FrozenArray<T>` — a GET-ONLY property (the section's own prose: "`FrozenArray<T>`
// has no mutating member"). Language spec §23.3 (v0.359) makes a property
// with no setter "a read-only property … omits `set`", and §23.1 (v0.359)
// says a `readonly` auto-property's assignment "is a store to the backing
// field (no setter exists to invoke, §22.7 rule 1)" and is admitted "only in
// a field initializer or in a constructor" — which a compiler-intrinsic
// `FrozenArray<T>` never has. So `f.Length := 3;` is a compile error.
//
// MEASURED on the base r30-c1 (`9ff2f909f`), on the issue's own program:
//
//     f.Length := 3;   ->  COMPILED (exit 0) with exactly one diagnostic:
//                          "internal: assignment target lvalue address failed
//                           to resolve (store dropped — target retains its
//                           previous value) — #4045"
//
// i.e. a SILENT ACCEPTANCE: the store was DROPPED and the program ran.
//
// THE CAUSE. `FrozenArray<T>.Length` is a BUILTIN member: it resolves to no
// `PropertySymbol` (the builtin collection members are typed by
// `common::isBuiltinIntLengthPropertyHead`, not by a property symbol), so
// `ExprTC::checkPropertyWriteTarget` — where every write-target rule lives —
// found no symbol and skipped every rule. The write then reached codegen,
// where a builtin member has no lvalue address to store into, and the store
// was dropped.
//
// THE FIX. The same function now rejects a write to a FrozenArray receiver's
// `Length` with the compiler's ORDINARY read-only-property diagnostic —
// byte-identical in shape to the one a user get-only property already gets:
//
//     cannot assign to read-only property `Length` — it has no setter (§23.3)
//
// The receiver predicate is the FrozenArray INDEXER rule's own (fix #1723,
// §4.19 / §30.14), so BOTH TypeRef shapes of a FrozenArray receiver are
// covered: the bare annotation `var f: FrozenArray<int>` (a Named+astNode)
// and the factory result `FrozenArray<int>.From(...)` (a Named+declaration).
//
// Every rejection below asserts on diagnostic TEXT, never on the coarse
// "did not run" signal: runAot's WIFSIGNALED branch synthesizes an Error for
// a runtime SIGSEGV too, so a bare `!ran` cannot tell a genuine rejection
// from a crash.
//
// Spec: stdlib §4.19 (the `FrozenArray<T>` declaration block —
// `public readonly property Length: int;`), language spec §23.3 (the
// read-only-property rule), §23.1 (a `readonly` auto-property's assignable
// context), §17.6 ("Read-only locations cannot be incremented"), §6.6 (the
// compound-assignment family). Spec revision v0.359.
//
// The `?.` receiver twin (`f?.Length := 3;`) is NOT a row here: A.6's lvalue
// shape already rejects that spelling with its own §7.5 diagnostic, so the
// fix does not reach it — it is left in the lane's `corpus-candidates/` with
// the deciding sentence. The read twin (`f.Length` as a READ) is already
// pinned by `tests/ownership/frozen_array_length_captured_lambda_9185_test.cpp`
// (four rows), which this fix leaves green.
//
// Model: none — the spec decides the case (stdlib §4.19 + §23.3).

#include <initializer_list>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff an ERROR diagnostic contains every one of `needles`.
bool errorSays(const std::string& program,
               std::initializer_list<const char*> needles) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* n : needles)
            if (d.message.find(n) == std::string::npos) { all = false; break; }
        if (all) return true;
    }
    return false;
}

// stdlib §4.19's `FrozenArray<T>` local, built by the section's own
// `From(items: Array<T>)` factory (the "copies" overload — the only
// construction form for `T = string`).
const char* kFrozenLocal =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<int>(); s.Add(7); s.Add(2);\n"
    "    using var f := FrozenArray<int>.From(s);\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same type reached through a FIELD (the `h.F.Length := 3;` shape).
const char* kFrozenField =
    "module P; import System;\n"
    "public class Holder\n"
    "  public F: FrozenArray<int>;\n"
    "  public constructor(f: FrozenArray<int>) this.F := f; end constructor;\n"
    "end Holder;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<int>(); s.Add(7); s.Add(2);\n"
    "    using var f := FrozenArray<int>.From(s);\n"
    "    var h := new Holder(f);\n"
    "    %s\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The FACTORY-CALL receiver: the factory result used directly as the base of
// the lvalue chain (A.6's lvalue shape forbids that base — see row 2).
const char* kFrozenFactoryReceiver =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var s := new Array<int>(); s.Add(7); s.Add(2);\n"
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

}  // namespace

// stdlib §4.19 + §23.3: the FrozenArray LOCAL's `Length` is get-only, so the
// write is rejected with the ordinary read-only-property diagnostic.
BEZEL_TEST(frozen_array_length_readonly_property_9214,
           local_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(errorSays(
        with(kFrozenLocal, "f.Length := 3;"),
        {"assign to", "read-only property `Length`", "no setter", "23.3"}));
}

// stdlib §4.19 + §23.3: the same write through a FIELD's FrozenArray. The
// receiver is one hop further out, but the property is the same get-only one.
BEZEL_TEST(frozen_array_length_readonly_property_9214,
           field_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(errorSays(
        with(kFrozenField, "h.F.Length := 3;"),
        {"assign to", "read-only property `Length`", "no setter", "23.3"}));
}

// stdlib §4.19 + §23.3: the FACTORY-CALL receiver. A.6's lvalue shape
// already rejects this spelling ("a method-call result cannot be the base of
// an assignment target", §6.6) — that diagnostic is UNCHANGED by this fix,
// and the read-only-property rule now fires on it as well.
BEZEL_TEST(frozen_array_length_readonly_property_9214,
           factory_receiver_length_assignment_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(errorSays(
        with(kFrozenFactoryReceiver,
             "FrozenArray<int>.From(s).Length := 3;"),
        {"assign to", "read-only property `Length`", "no setter", "23.3"}));
    // The §6.6 lvalue-shape diagnostic is unchanged by this fix.
    BEZEL_EXPECT_TRUE(errorSays(
        with(kFrozenFactoryReceiver,
             "FrozenArray<int>.From(s).Length := 3;"),
        {"method-call result cannot be the base of an assignment target",
         "6.6"}));
}

// §17.6: "The operand must be an assignable lvalue … Read-only locations
// cannot be incremented." The increment spelling of the same write reaches
// the same check (the `incOp` argument of the shared check) and is rejected
// with the increment wording.
BEZEL_TEST(frozen_array_length_readonly_property_9214,
           length_increment_rejected_spec_17_6) {
    BEZEL_EXPECT_TRUE(errorSays(
        with(kFrozenLocal, "f.Length++;"),
        {"increment/decrement", "read-only property `Length`", "no setter",
         "17.6", "23.3"}));
}

// §6.6: the compound family (`x op= e` is `x := x op e`) reaches the same
// write-target check and takes the same rejection.
BEZEL_TEST(frozen_array_length_readonly_property_9214,
           length_compound_assignment_rejected_spec_6_6) {
    BEZEL_EXPECT_TRUE(errorSays(
        with(kFrozenLocal, "f.Length += 1;"),
        {"assign to", "read-only property `Length`", "no setter", "23.3"}));
}
