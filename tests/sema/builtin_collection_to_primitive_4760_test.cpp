// Regression tests for issue #4760 — a local annotated with a PRIMITIVE type
// and initialized from a BUILT-IN HEAP COLLECTION was accepted with NO
// diagnostic at all, and the local then read UNINITIALIZED stack memory.
//
// §5.1 (v0.307) enumerates the implicit conversions; there is no row between a
// class reference and a value type, and §4.6 forbids an implicit conversion to
// `string`. `checkAssignable`'s #1178 gate already states exactly that rule —
// "There is no implicit conversion between a class reference and a primitive"
// — but is restricted to `from->declaration != nullptr`, and the §4 heap
// collections carry `declaration == nullptr` (the spec types are synthesized,
// there is no TypeSymbol), so they fell through every branch to the
// "Mixed / generic-parameter / null-literal cases: skip" catch-all.
//
// MEASURED on the pre-fix tree (6f1bfc4a) with the real `build/bezelc`:
//
//     using var a := new Array<int>();
//     a.Add(5);
//     var x: int := a;              // §5.1: expected an ERROR
//     Console.WriteLine("survived");
//     Console.WriteLine(x);
//
//   $ bezelc -o n1 n1.bzl && ./n1; ./n1; ./n1
//   survived / 349225760
//   survived / 908358432
//   survived / 1068782368
//
// Exit 0, no warning, not even an `internal:` note, and a DIFFERENT value on
// every run — genuine UB, not #2434's deterministic zero. `Dictionary<K,V>`,
// `Set<T>` and `LinkedList<T>` behaved identically; `Stack<T>` into a `bool`
// target reached codegen and aborted the IR verifier instead ("Trunc only
// operates on integer / %7 = trunc ptr %6 to i1") — the same missing
// rejection, a different downstream symptom.
//
// ⚠ ORACLE SHAPE. Because the wrong value VARIES between runs, no row here may
// assert a specific wrong number. The reject rows assert the §5.1 rejection
// AND the rule's own wording (a UNIQUE substring, not the bare section number
// — a short "5.1" needle matches surrounding text and would pass vacuously if
// an EARLIER check started rejecting the same input for a different reason,
// the #4716 shape). The accept-side controls RUN and assert exact stdout.
//
// Cite: spec §5.1, §4.6, v0.307; stdlib §4.5 / §4.8 / §4.9 / §4.10 / §4.11.

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// The rule's OWN wording. Unique in src/ (one occurrence, ExprTypecheck.cpp) —
// deliberately not the bare "§5.1", which appears in the generic head of every
// not-convertible diagnostic and in many neighbouring ones.
constexpr const char* kRuleWording =
    "is a heap reference and the target is a value type";

// True when the program was REJECTED and at least one error carries the §5.1
// rule's own wording. Both halves matter: rejection alone is satisfied by any
// unrelated error (and by the pre-fix `Stack`/`bool` row, which "failed" at the
// IR verifier), so the wording is what pins THIS rule.
bool rejectedByRule(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kRuleWording) != std::string::npos) return true;
    }
    return false;
}

std::string localDeclRepro(const std::string& ctorLine,
                           const std::string& annotation) {
    return
        "module Conf;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var a := " + ctorLine + ";\n"
        "    var x: " + annotation + " := a;\n"
        "    Console.WriteLine(\"survived\");\n"
        "  end Main;\n"
        "end Program;\n";
}

}  // namespace

// ═════════ Primary bug — the issue's measured scope table, head by head ═════
// Every row here compiled clean and read uninitialized memory pre-fix.

BEZEL_TEST(builtin_collection_to_primitive_4760,
           array_into_int_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(localDeclRepro("new Array<int>()", "int")));
}

BEZEL_TEST(builtin_collection_to_primitive_4760,
           dictionary_into_int_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        localDeclRepro("new Dictionary<string, int>()", "int")));
}

// Pre-fix this one did NOT compile clean — it reached codegen and aborted the
// IR verifier ("Trunc only operates on integer"). A bare `!r.ran` assertion
// would therefore have passed on the pre-fix compiler too; the rule-wording
// half of `rejectedByRule` is what makes this row fail without the fix.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           stack_into_bool_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(localDeclRepro("new Stack<int>()", "bool")));
}

BEZEL_TEST(builtin_collection_to_primitive_4760,
           set_into_int_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(localDeclRepro("new Set<int>()", "int")));
}

BEZEL_TEST(builtin_collection_to_primitive_4760,
           linked_list_into_int_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(
        localDeclRepro("new LinkedList<int>()", "int")));
}

// §4.6: no implicit conversion to `string` either. `string` is a Primitive in
// this type model, so it reaches the same gate.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           array_into_string_local_rejected_spec_4_6) {
    BEZEL_EXPECT_TRUE(
        rejectedByRule(localDeclRepro("new Array<int>()", "string")));
}

// A NULLABLE primitive target is the same ill-formed conversion — §5.1's
// nullable conversions relate `T` to `T?`, not a class reference to `int?`.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           array_into_nullable_int_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(localDeclRepro("new Array<int>()", "int?")));
}

// ═════════ The other §5.1 conversion positions, all measured accepted ══════
// `checkAssignable` is the ONE §5.1 conversion site, so the fix reaches every
// position at once. Each of these compiled clean pre-fix (the argument form
// aborted the IR verifier with "Call parameter type does not match function
// signature!" instead).

BEZEL_TEST(builtin_collection_to_primitive_4760,
           array_assigned_to_int_variable_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Program
  public static method Main()
    using var a := new Array<int>();
    var x: int := 1;
    x := a;
    Console.WriteLine(x);
  end Main;
end Program;
)BZ"));
}

BEZEL_TEST(builtin_collection_to_primitive_4760,
           array_passed_to_int_parameter_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Program
  public static method Take(v: int): int
    return v;
  end Take;
  public static method Main()
    using var a := new Array<int>();
    Console.WriteLine(Program.Take(a));
  end Main;
end Program;
)BZ"));
}

BEZEL_TEST(builtin_collection_to_primitive_4760,
           array_returned_as_int_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Program
  public static method Give(a: Array<int>): int
    return a;
  end Give;
  public static method Main()
    using var a := new Array<int>();
    Console.WriteLine(Program.Give(a));
  end Main;
end Program;
)BZ"));
}

// ═════════ Beyond the §4 collections — every built-in heap reference ═══════
// The gate reads `common::isHeapPointerReferenceBuiltin`, the shared authority
// for "this value is a heap pointer to a runtime object", so the §3 nullary
// classes and the §29 iteration interfaces are in the same population.

BEZEL_TEST(builtin_collection_to_primitive_4760,
           string_builder_into_int_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(
        rejectedByRule(localDeclRepro("new StringBuilder()", "int")));
}

BEZEL_TEST(builtin_collection_to_primitive_4760,
           ienumerable_into_int_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Program
  public static method Main()
    using var a := new Array<int>();
    var e: IEnumerable<int> := a;
    var x: int := e;
    Console.WriteLine("survived");
  end Main;
end Program;
)BZ"));
}

// ═════════ The independently-reported `Drain()` rows ════════════════
// Reported separately from #4760 while probing another defect, with the
// hypothesis that the §5.1 check is skipped because `pool.Drain()` is an
// "untyped call expression". MEASURED with a temporary dump inside
// `checkAssignable`: it is NOT untyped — the site is entered with
// `from = Named, declaration = NULL, head = IOwningEnumerable`, byte-for-byte
// the same shape as the `Array` row above, and zero null-source events occur
// in either program. So these two rows are #4760, not a second mechanism, and
// they are pinned here because they widen the population past the concrete
// §4 collections to the §4.13 owning-enumeration interface.
//
// Pre-fix: the `int` form compiled clean behind a `#2434` codegen note, and
// the `string` form compiled clean with NO diagnostic at all.
//
// (A genuinely unresolved source type IS a separate, still-open hole —
// `checkAssignable` returns at its first line on `!from`, and 1,229 of the
// 21,921 test-tree programs compile clean while reaching at least one such
// site. That is filed, not fixed here.)

BEZEL_TEST(builtin_collection_to_primitive_4760,
           owning_array_drain_into_int_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Item
  public constructor()
  end constructor;
end Item;
public class Program
  public static method Main()
    using var pool := new OwningArray<Item>();
    pool.Add(new Item());
    using var view: int := pool.Drain();
    Console.WriteLine("survived");
  end Main;
end Program;
)BZ"));
}

BEZEL_TEST(builtin_collection_to_primitive_4760,
           owning_array_drain_into_string_local_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Item
  public constructor()
  end constructor;
end Item;
public class Program
  public static method Main()
    using var pool := new OwningArray<Item>();
    pool.Add(new Item());
    using var view: string := pool.Drain();
    Console.WriteLine("survived");
  end Main;
end Program;
)BZ"));
}

// ═════════ Accept-side controls — these RUN and assert exact stdout ════════
// A reject rule's real risk is over-rejection, and a compile-only control
// cannot see a value that silently degrades, so every control here executes.

// The remedy the diagnostic names: read a MEMBER of the collection. This is
// the shape the rejected programs were almost certainly reaching for.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           count_and_index_reads_still_accepted_control_stdlib_4_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var x: int := a.Count;
    Console.WriteLine(x);
    var y: int := a[1];
    Console.WriteLine(y);
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n6\n"));
}

// §8.6 boxing: an `Object` target is handled by `checkAssignable`'s `Object`
// early-return, far above the new gate, and must stay accepted.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           array_boxed_into_object_still_accepted_control_spec_8_6) {
    auto r = runAot(R"BZ(
module Conf;
public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    var o: Object := a;
    Console.WriteLine("boxed");
    Console.WriteLine(a.Count);
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("boxed\n1\n"));
}

// §8.5: the EXPLICIT conversion to `string` is `ToString()`, and it is
// unaffected — the gate fires on the implicit conversion only.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           explicit_tostring_still_accepted_control_spec_8_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    var s: string := a.ToString();
    Console.WriteLine(s.Length > 0);
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// ═════════ Scope controls — the shapes deliberately left OUT ═══════════════

// A built-in whose TypeRef carries a `declaration` (the builtinClassSym shape
// `FrozenArray<T>.From(...)` stamps) was ALREADY rejected pre-fix by #1178's
// gate, with the generic §5.1 wording. This row passes on both sides; it pins
// that the fix did not re-route an existing rejection through the new arm.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           frozen_array_rejection_unchanged_control_spec_5_1) {
    auto r = runAot(R"BZ(
module Conf;
public class Program
  public static method Main()
    var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);
    using var fa := FrozenArray<int>.From(move buf);
    var x: char := fa;
    Console.WriteLine("survived");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.ran);
    bool generic = false, viaNewArm = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("not implicitly convertible to the target type") !=
            std::string::npos)
            generic = true;
        if (d.message.find(kRuleWording) != std::string::npos) viaNewArm = true;
    }
    BEZEL_EXPECT_TRUE(generic);
    BEZEL_EXPECT_FALSE(viaNewArm);
}

// A user class was already rejected the same way, by the same #1178 gate.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           user_class_rejection_unchanged_control_spec_5_1) {
    auto r = runAot(R"BZ(
module Conf;
public class Widget
  public constructor()
  end constructor;
end Widget;
public class Program
  public static method Main()
    using var w := new Widget();
    var x: int := w;
    Console.WriteLine("survived");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.ran);
    bool viaNewArm = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kRuleWording) != std::string::npos) viaNewArm = true;
    }
    BEZEL_EXPECT_FALSE(viaNewArm);
}

// The RIGHT-annotation control for the `Drain()` rows: annotated at its own
// declared type (stdlib §4.5 `Drain(): IOwningEnumerable<T>`) the same call is
// accepted and RUNS, draining both elements. This is the probe that separates
// "sema never typed it" from "sema typed it and §5.1 swallowed the mismatch" —
// a wrong-annotation accept is ambiguous between the two, a right-annotation
// run is not.
BEZEL_TEST(builtin_collection_to_primitive_4760,
           owning_array_drain_right_annotation_runs_control_stdlib_4_5) {
    auto r = runAot(R"BZ(
module Conf;
public class Item
  public constructor()
  end constructor;
end Item;
public class Program
  public static method Main()
    using var pool := new OwningArray<Item>();
    pool.Add(new Item());
    pool.Add(new Item());
    using var view: IOwningEnumerable<Item> := pool.Drain();
    var n: int := 0;
    for var it in view do
      n := n + 1;
      it.Dispose();
    end for;
    Console.WriteLine(n);
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}
