// Spec-derived regression test for issue #6778 (owner ruling, v0.340), and the
// Console/Object part of #6811 that points to it.
//
// stdlib §2.1 declares `Console.Write(value: string)`, `Write(value: Object)`,
// `WriteLine(value: string)` and `WriteLine(value: Object)`, plus per-type
// bool/numeric overloads; stdlib §7.4 gives `TextWriter` (`Console.Out`,
// `Console.Error`) the same set. None of them is nullable. So:
//   - §7.1: a `T?` does not convert to `T`, so the `string` overload (and the
//     per-type ones) are inapplicable to an un-narrowed `T?`;
//   - §5.1: a nullable `T?` "converts at most to `Object?`, never to `Object`
//     ... so an un-narrowed `Animal?` is rejected at an `Object` parameter";
//   - §22.6: a `null` literal "is applicable at every nullable parameter and
//     at no non-nullable one".
// No candidate is applicable, which is a compile error. The ruling: "sema
// rejects an un-narrowed nullable argument at these non-nullable
// `Console`/`TextWriter` parameters, as it already does at user-declared
// ones." Before the fix every reject row below compiled: `Console.WriteLine(s)`
// on a null `string?` printed an empty line, and on a null `Object?` or a
// `null` literal printed `null` (issue probes m23/m24).
//
// The remedies stay accepted: a §7.4-narrowed read, `??`, `!`, and an
// interpolation hole. The ruling names the hole explicitly: "`$"{s}"` renders
// `null` for an absent value (§18.5), which is the spelling that prints a
// possibly-absent value". A hole's string is a non-nullable `string`, so
// nullable interpolation pieces are NOT in scope of the rejection.
//
// Reject rows assert the exact §7 text the #6782 built-in null check gives,
// at the argument's line and column. Accept rows assert the exact stdout.

#include <cstdint>
#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The AOT harness reports a compile reject as ONE Error diagnostic carrying
// the driver's stderr, whose lines read `<path>:<line>:<col>: <message>`.
// True iff that stderr holds `message` at exactly `line`:`column`.
bool rejectsWith(const std::string& src, std::uint32_t line,
                 std::uint32_t column, const std::string& message) {
    auto r = runAot(src);
    if (r.ran) return false;
    const std::string want = ":" + std::to_string(line) + ":" +
                             std::to_string(column) + ": " + message + "\n";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            (d.message + "\n").find(want) != std::string::npos)
            return true;
    return false;
}

// The exact §7 text the #6782 built-in null check gives for `callee`.
std::string nullableArgMessage(std::string_view callee) {
    return std::string("the argument to `") + std::string(callee) +
           "` is nullable but the target type is non-nullable (\xC2\xA7"
           "7)";
}

// True iff `src` is rejected with EXACTLY the §7 nullable-argument error for
// `callee`, reported at the argument of the first occurrence of `call` (the
// position just after its first `(`).
bool rejectsAt(const std::string& src, std::string_view call,
               std::string_view callee) {
    const std::size_t at = src.find(call);
    if (at == std::string::npos) return false;
    const std::size_t paren = call.find('(');
    if (paren == std::string_view::npos) return false;
    const std::size_t argAt = at + paren + 1;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    for (std::size_t i = 0; i < argAt; ++i) {
        if (src[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    const std::string message = nullableArgMessage(callee);
    return rejectsWith(src, line, column, message);
}

}  // namespace

// ===========================================================================
// REJECT rows: compiled on base, now the §7 error.
// ===========================================================================

// §22.6: the `null` literal is applicable at no non-nullable parameter.
BEZEL_TEST(console_nullable_arg_6778, null_literal_writeline_rejected_22_6) {
    const std::string src = R"(module M;
class C
  public static method Main(): int
    Console.WriteLine(null);
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(
        rejectsAt(src, "Console.WriteLine(null)", "Console.WriteLine"));
}

// The issue's probe m23: §7.4 establishes no narrowing from a nullable-typed
// initializer, and §7.1 `string?` is not `string`. Base printed an empty line.
BEZEL_TEST(console_nullable_arg_6778, m23_null_string_q_rejected_7_1) {
    const std::string src = R"(module M;
class C
  public static method Main(): int
    var s: string? := null;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(
        rejectsAt(src, "Console.WriteLine(s)", "Console.WriteLine"));
}

// The issue's probe m24: §5.1 `Object?` never converts to `Object`. Base
// printed `null`.
BEZEL_TEST(console_nullable_arg_6778, m24_null_object_q_rejected_5_1) {
    const std::string src = R"(module M;
class C
  public static method Main(): int
    var o: Object? := null;
    Console.WriteLine(o);
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(
        rejectsAt(src, "Console.WriteLine(o)", "Console.WriteLine"));
}

// `Console.Write(o)` with an un-narrowed `Object?` parameter (§5.1).
BEZEL_TEST(console_nullable_arg_6778, write_object_q_param_rejected_5_1) {
    const std::string src = R"(module M;
class C
  public static method Show(o: Object?)
    Console.Write(o);
  end Show;
  public static method Main(): int
    C.Show("x");
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(rejectsAt(src, "Console.Write(o)", "Console.Write"));
}

// A nullable record (§7.2 nullable value type) at `WriteLine` (§5.1, §7.1).
BEZEL_TEST(console_nullable_arg_6778, nullable_record_rejected_5_1) {
    const std::string src = R"(module M;
record P(X: int, Y: int);
class C
  public static method Show(p: P?)
    Console.WriteLine(p);
  end Show;
  public static method Main(): int
    C.Show(new P(1, 2));
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(
        rejectsAt(src, "Console.WriteLine(p)", "Console.WriteLine"));
}

// An `int?` at `WriteLine`: the per-type `int` overload takes `int` (§7.1).
BEZEL_TEST(console_nullable_arg_6778, nullable_int_rejected_7_1) {
    const std::string src = R"(module M;
class C
  public static method Show(i: int?)
    Console.WriteLine(i);
  end Show;
  public static method Main(): int
    C.Show(5);
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(
        rejectsAt(src, "Console.WriteLine(i)", "Console.WriteLine"));
}

// A nullable class, enum, bool and double in one body: each argument is
// reported (the row checks the class one's position; the others are counted).
BEZEL_TEST(console_nullable_arg_6778, nullable_class_enum_bool_double_rejected_7_1) {
    const std::string src = R"(module M;
enum Color
  Red, Green;
end Color;
class K end K;
class C
  public static method Show(k: K?, e: Color?, b: bool?, d: double?)
    Console.WriteLine(k);
    Console.WriteLine(e);
    Console.WriteLine(b);
    Console.Write(d);
  end Show;
  public static method Main(): int
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(
        rejectsAt(src, "Console.WriteLine(k)", "Console.WriteLine"));
    // The other three arguments, each at its own line (column 23 after
    // `    Console.WriteLine(`, 19 after `    Console.Write(`).
    BEZEL_EXPECT_TRUE(rejectsWith(src, 9, 23,
                                  nullableArgMessage("Console.WriteLine")));
    BEZEL_EXPECT_TRUE(rejectsWith(src, 10, 23,
                                  nullableArgMessage("Console.WriteLine")));
    BEZEL_EXPECT_TRUE(
        rejectsWith(src, 11, 19, nullableArgMessage("Console.Write")));
}

// `Console.Out` and `Console.Error` are `TextWriter`s with the same
// non-nullable set (stdlib §7.4, §2.1).
BEZEL_TEST(console_nullable_arg_6778, out_and_error_facets_rejected_7_4) {
    const std::string outSrc = R"(module M;
class C
  public static method Show(s: string?)
    Console.Out.WriteLine(s);
  end Show;
  public static method Main(): int
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(rejectsAt(outSrc, "Console.Out.WriteLine(s)",
                                "Console.Out.WriteLine"));
    const std::string errSrc = R"(module M;
class C
  public static method Main(): int
    Console.Error.Write(null);
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(rejectsAt(errSrc, "Console.Error.Write(null)",
                                "Console.Error.Write"));
}

// A named `value := s` binds the declared parameter `value` (§22.5).
BEZEL_TEST(console_nullable_arg_6778, named_value_argument_rejected_22_5) {
    const std::string src = R"(module M;
class C
  public static method Show(s: string?)
    Console.WriteLine(value := s);
  end Show;
  public static method Main(): int
    return 0;
  end Main;
end C;
)";
    // Reported at the value expression after `value := ` (line 4, col 32).
    BEZEL_EXPECT_TRUE(rejectsWith(src, 4, 32,
                                  nullableArgMessage("Console.WriteLine")));
}

// #6811's Console repro, verbatim: a named `null` literal. Pre-fix it
// compiled and printed `null` (§7.3: null is not assignable to a
// non-nullable target).
BEZEL_TEST(console_nullable_arg_6778, issue_6811_named_null_literal_rejected_7_3) {
    const std::string src = R"(module M;
class C
  public static method Main(): int
    Console.WriteLine(value := null);
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(rejectsWith(src, 4, 32,
                                  nullableArgMessage("Console.WriteLine")));
}

// §7.5: "The result type is always nullable" for `x?.M`.
BEZEL_TEST(console_nullable_arg_6778, null_conditional_result_rejected_7_5) {
    const std::string src = R"(module M;
class C
  public static method Show(s: string?)
    Console.WriteLine(s?.Length);
  end Show;
  public static method Main(): int
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(rejectsAt(src, "Console.WriteLine(s?.Length)",
                                "Console.WriteLine"));
}

// stdlib §2.1 `ReadLine(): string?` passed straight through.
BEZEL_TEST(console_nullable_arg_6778, readline_result_rejected_7_1) {
    const std::string src = R"(module M;
class C
  public static method Main(): int
    Console.WriteLine(Console.ReadLine());
    return 0;
  end Main;
end C;
)";
    BEZEL_EXPECT_TRUE(rejectsAt(src, "Console.WriteLine(Console.ReadLine())",
                                "Console.WriteLine"));
}

// The #5240 shape: an un-narrowed `T?` in a generic body (§7.1, §22.6).
BEZEL_TEST(console_nullable_arg_6778, generic_t_q_rejected_7_1) {
    const std::string src = R"(module M;
class G
  public static method Show<T>(v: T?)
    var q: T? := v;
    Console.WriteLine(q);
  end Show;
end G;
class P
  public static method Main(): int
    G.Show<int>(5);
    return 0;
  end Main;
end P;
)";
    BEZEL_EXPECT_TRUE(
        rejectsAt(src, "Console.WriteLine(q)", "Console.WriteLine"));
}

// §7.4: a mutably-captured local loses its narrowing at an intervening call
// (conformance case s7_4_0010, which this fix turns from a divergence into a
// pass).
BEZEL_TEST(console_nullable_arg_6778, mutably_captured_loses_narrowing_7_4) {
    const std::string src = R"(module M;
public class Program
  public static method Noisy() => Console.WriteLine("noisy");
  public static method Main()
    var label: string? := "gizmo";
    var clear := lambda() => do label := null; end;
    if label <> null then
      Noisy();
      Console.WriteLine(label);
    end;
    clear();
  end Main;
end Program;
)";
    BEZEL_EXPECT_TRUE(
        rejectsAt(src, "Console.WriteLine(label)", "Console.WriteLine"));
}

// ===========================================================================
// ACCEPT rows: the ruling's remedies. Exact stdout, unchanged from base.
// ===========================================================================

// §7.4 narrowing by guard, by non-null initializer and by early return; `!`.
BEZEL_TEST(console_nullable_arg_6778, narrowed_reads_accepted_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(R"(module M;
record P(X: int, Y: int);
class C
  public static method Early(s: string?)
    if s = null then
      return;
    end if;
    Console.WriteLine(s);
  end Early;
  public static method Show(s: string?, o: Object?, i: int?, p: P?)
    if s <> null then Console.WriteLine(s); end;
    if o <> null then Console.WriteLine(o); end;
    if i <> null then Console.Write(i); Console.WriteLine(); end;
    if p <> null then Console.Out.WriteLine(p); end;
  end Show;
  public static method Main(): int
    C.Show("s", "o", 7, new P(1, 2));
    C.Show(null, null, null, null);
    var t: string? := "init";
    Console.WriteLine(t);
    C.Early("early");
    C.Early(null);
    var u: string? := "bang";
    Console.WriteLine(u!);
    return 0;
  end Main;
end C;
)"),
                    "s\no\n7\nP(X: 1, Y: 2)\ninit\nearly\nbang");
}

// `??` (§7.7) and an interpolation hole (§18.5, the ruling's named spelling
// for a possibly-absent value), both with absent operands.
BEZEL_TEST(console_nullable_arg_6778, coalesce_and_hole_accepted_7_7_18_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"(module M;
class C
  public static method Show(s: string?, o: Object?, i: int?)
    Console.WriteLine(s ?? "s-absent");
    Console.WriteLine(o ?? "o-absent");
    Console.WriteLine(i ?? -1);
    Console.WriteLine($"[{s}][{o}][{i}]");
    Console.Error.WriteLine(s ?? "e");
  end Show;
  public static method Main(): int
    C.Show(null, null, null);
    return 0;
  end Main;
end C;
)"),
                    "s-absent\no-absent\n-1\n[null][null][null]");
}

// Non-nullable arguments of every kind and the zero-argument `WriteLine()`.
BEZEL_TEST(console_nullable_arg_6778, non_nullable_and_zero_arg_accepted_2_1) {
    BEZEL_EXPECT_EQ(runAotOut(R"(module M;
record P(X: int, Y: int);
class K
  public override method ToString(): string => "K!";
end K;
class C
  public static method Main(): int
    var o: Object := new K();
    Console.WriteLine("lit");
    Console.WriteLine(42);
    Console.WriteLine(o);
    Console.WriteLine(new P(1, 2));
    Console.WriteLine();
    Console.Write(true);
    Console.Out.WriteLine();
    return 0;
  end Main;
end C;
)"),
                    "lit\n42\nK!\nP(X: 1, Y: 2)\n\nTrue");
}

// §7.4 "`readonly` fields narrow": a guarded nullable readonly field.
BEZEL_TEST(console_nullable_arg_6778, readonly_field_guard_accepted_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(R"(module M;
class G
  public readonly V: string?;
  public constructor(v: string?)
    V := v;
  end constructor;
end G;
class C
  public static method Main(): int
    var g := new G("ro");
    if g.V <> null then Console.WriteLine(g.V); end;
    return 0;
  end Main;
end C;
)"),
                    "ro");
}
