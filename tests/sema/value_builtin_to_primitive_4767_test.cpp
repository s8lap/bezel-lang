// Regression tests for issue #4767 — a VALUE-shaped reserved builtin
// (`common::kNonHeapReservedBuiltins`: `ArraySpan<T>`, `DateTime`, `TimeSpan`,
// `Guid`, `IntPtr`, `ReadOnlyStringSpan`, …) converted implicitly into a
// CONCRETE primitive was accepted with no diagnostic and read uninitialized
// memory.
//
// MEASURED (issue body, at e443f8dc — i.e. with #4760's fix):
//
//     var sp := a.AsSpan();          // a: Array<int>
//     var x: int := sp;              // §5.1: expected an ERROR
//   -> "survived / 670081824", then 171139872, then 542810912
//
//     var d: DateTime := DateTime.UtcNow;
//     var x: int := d;
//   -> "survived / -1458604580", then -1458591516, …
//
// Re-measured at r20-c25 (5a528344) with `bezelc -c`: every row below that is
// a REJECT row compiled clean (rc=0), into `int`, `long`, `bool` and `string`
// targets alike.
//
// §5.1 (v0.307) has no row from any of these types to a primitive: its table
// is primitive-to-primitive and its reference conversions end at `Object`
// (boxing) or an implemented interface. `IntPtr`/`UIntPtr` <-> `long`/`ulong`
// is EXPLICIT only (stdlib §10.0: "`IntPtr(longValue)` and `long(ptr)` use
// the standard explicit-conversion syntax (§5.2)").
//
// ⚠ ORACLE SHAPE. The wrong value varies between runs, so no row may assert a
// specific wrong number. Reject rows assert rejection AND the rule's own
// wording (unique in src/, not the bare "§5.1" — the #4716 vacuous-needle
// shape). Accept controls RUN and assert exact stdout.
//
// Cite: spec §5.1, §5.2, v0.307; stdlib §2.5, §3.3, §4.15, §10.0.

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <iostream>
#include <string>

using bezel::test::runAot;

namespace {

// The rule's OWN wording — one occurrence in src/ (ExprTypecheck.cpp).
constexpr const char* kRuleWording =
    "is a value type with no implicit conversion to a primitive";

bool rejectedByRule(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kRuleWording) != std::string::npos) return true;
    }
    return false;
}

std::string repro(const std::string& decl, const std::string& target) {
    return
        "module Conf;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var a := new Array<int>();\n"
        "    a.Add(5);\n"
        "    " + decl + "\n"
        "    var x: " + target + " := d;\n"
        "    Console.WriteLine(\"survived\");\n"
        "  end Main;\n"
        "end Program;\n";
}

// The declaration-less shape: written annotations and the span factories.
// (The builtinClassSym shape — `t.Date`, `TimeSpan.FromSeconds(…)` unannotated
// — was already rejected by #1178's gate, and `TimeSpan.Zero` / `DateTime.
// UtcNow` unannotated are UNTYPED sources, #4766's residual.)
const char* const kDecls[] = {
    "var d := a.AsSpan();",                               // ArraySpan<int>
    "var d := a.AsReadOnlySpan();",                       // ReadOnlyArraySpan<int>
    "var d := \"abc\".AsSpan();",                         // ReadOnlyStringSpan
    "var d: DateTime := DateTime.UtcNow;",                // DateTime (annotated)
    "var d: TimeSpan := TimeSpan.FromSeconds(2.0);",      // TimeSpan (annotated)
    "var d: Guid := Guid.NewGuid();",                     // Guid
    "var d: IntPtr := IntPtr.Zero;",                      // IntPtr (§10.0)
};

void expectAllRejected(const char* target) {
    for (const char* decl : kDecls) {
        bool ok = rejectedByRule(repro(decl, target));
        if (!ok) std::cout << "    row: " << decl << " -> " << target << "\n";
        BEZEL_EXPECT_TRUE(ok);
    }
}

}  // namespace

BEZEL_TEST(value_builtin_to_primitive_4767, into_int_rejected_spec_5_1) {
    expectAllRejected("int");
}

// `long` is the one target a numeric bridge exists for (`IntPtr`), and that
// bridge is EXPLICIT (stdlib §10.0 / §5.2).
BEZEL_TEST(value_builtin_to_primitive_4767, into_long_rejected_spec_5_2) {
    expectAllRejected("long");
}

BEZEL_TEST(value_builtin_to_primitive_4767, into_bool_rejected_spec_5_1) {
    expectAllRejected("bool");
}

// §4.6: no implicit conversion to `string` (the explicit form is ToString()).
BEZEL_TEST(value_builtin_to_primitive_4767, into_string_rejected_spec_4_6) {
    expectAllRejected("string");
}

// Nullable target, argument position, return position.
BEZEL_TEST(value_builtin_to_primitive_4767, other_positions_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(repro("var d := a.AsSpan();", "int?")));
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Program
  public static method Take(n: int): int
    return n;
  end Take;
  public static method Main()
    var d: DateTime := DateTime.UtcNow;
    Console.WriteLine(Take(d));
  end Main;
end Program;
)BZ"));
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Program
  public static method Ticks(d: DateTime): long
    return d;
  end Ticks;
  public static method Main()
    Console.WriteLine(Ticks(DateTime.UtcNow));
  end Main;
end Program;
)BZ"));
}

// ═════════ Accept controls — the sanctioned spellings still run ═════════════

// Reading a member, the explicit `long(ptr)` conversion (stdlib §10.0), and
// boxing into `Object` (§5.1 "any type to Object") are all unaffected.
BEZEL_TEST(value_builtin_to_primitive_4767, sanctioned_spellings_run_control) {
    auto r = runAot(R"BZ(
module Conf;
public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var sp := a.AsSpan();
    var n: int := sp.Length;
    var ts: TimeSpan := TimeSpan.FromSeconds(2.0);
    var secs: int := ts.Seconds;
    var p: IntPtr := IntPtr.Zero;
    var raw: long := long(p);
    var o: Object := ts;
    Console.WriteLine(n);
    Console.WriteLine(secs);
    Console.WriteLine(raw);
    Console.WriteLine(o is TimeSpan);
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n2\n0\nTrue\n"));
}

// A type PARAMETER that shadows a value-builtin name is not the builtin: the
// arm must not claim it (it is #4768's shape, with #4768's own wording).
BEZEL_TEST(value_builtin_to_primitive_4767, shadowing_type_parameter_not_claimed) {
    auto r = runAot(R"BZ(
module Conf;
public class Box<DateTime>
  public method Pick(v: DateTime): int
    var t: int := v;
    return t;
  end Pick;
end Box;
public class Program
  public static method Main()
    using var b := new Box<int>();
    Console.WriteLine(b.Pick(42));
  end Main;
end Program;
)BZ");
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find(kRuleWording) == std::string::npos);
}
