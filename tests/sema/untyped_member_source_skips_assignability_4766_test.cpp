// Regression tests for issue #4766 (narrowed) — `checkAssignable` returns
// early (`if (!from || !to) return;`) when the SOURCE expression carries no
// resolved type, so every §5.1 check is skipped for it. The most common
// untyped sources in a compile-clean program were `int`-valued extent/count
// property reads on built-in heap types that no member-typing path handled.
//
// MEASURED on the pre-fix tree (r20-c25, 5a528344) with the real `bezelc -c`:
//
//     var sb := new StringBuilder();
//     var b: bool := sb.Length;     // §5.1: int -> bool is never implicit
//
//   rc=0, no diagnostic (§5.1: "between integers and `bool` is never
//   implicit"). `var s: string := sb.Length;` was likewise accepted (§4.6: no
//   implicit conversion to `string`). Identical for `OwningBuffer<T>.Length`,
//   an annotated `FrozenArray<T>.Length`, and `Count` on `SortedSet<T>`,
//   `SortedDictionary<K, V>` and `Channel<T>`.
//
// The fix types those reads `int` (stdlib §3.1, §4.14, §4.17, §4.18, §4.19,
// §6.9 declare each `readonly property … : int`). Every row here uses only a
// stdlib-DOCUMENTED member (CLAUDE.md rule 2): the fix also types the
// undocumented `Length` alias on Array/Stack/Queue, but stdlib §4.4 gives
// those types `Count` only, so no row depends on it (the alias is #4811).
// It is a NARROW fix: the early return
// itself stays, because most null-source events are target-typed forms that
// are checked elsewhere (collection literals, throw expressions, `new()`).
//
// ORACLE. Each reject row asserts rejection AND a substring that names THIS
// conversion — `initializer for `b`` plus the target-specific tail — so an
// unrelated earlier error cannot satisfy it. Both an `int`→`bool` and an
// `int`→`string` target are pinned (the #4760 lesson: the defect is keyed on
// the untyped SOURCE, not on the target). The accept control RUNS and asserts
// exact stdout, since typing these reads changes what codegen sees.
//
// Cite: spec §5.1, §4.6, v0.307; stdlib §3.1, §4.14, §4.17, §4.18, §4.19,
// §6.9.

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <iostream>
#include <string>

using bezel::test::runAot;

namespace {

// §5.1 head produced by checkAssignable for the `var b: T := …` initializer.
constexpr const char* kHead =
    "the initializer for `b` has a type that is not implicitly convertible "
    "to the target type (\xC2\xA7" "5.1)";
// The int -> bool tail (numeric table: bool is never an implicit target).
constexpr const char* kBoolTail = "between integers and `bool` is never implicit";

bool rejectedWith(const std::string& program, const char* extra) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kHead) == std::string::npos) continue;
        if (extra && d.message.find(extra) == std::string::npos) continue;
        return true;
    }
    return false;
}

std::string repro(const std::string& decl, const std::string& expr,
                  const std::string& target) {
    return
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    " + decl + "\n"
        "    var b: " + target + " := " + expr + ";\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

struct Row { const char* decl; const char* expr; };

const Row kRows[] = {
    {"var a := new StringBuilder();", "a.Length"},
    {"using var a := new OwningBuffer<int>(3);", "a.Length"},
    {"var a: FrozenArray<int> := FrozenArray<int>.From([1, 2]);", "a.Length"},
    {"var a := new SortedSet<int>();", "a.Count"},
    {"var a := new SortedDictionary<int, int>();", "a.Count"},
    {"using var a := new Channel<int>();", "a.Count"},
};

}  // namespace

// The issue's repro shape: an int-valued built-in property into `bool`.
BEZEL_TEST(untyped_member_source_skips_assignability_4766,
           int_property_into_bool_rejected_spec_5_1) {
    for (const auto& row : kRows) {
        bool ok = rejectedWith(repro(row.decl, row.expr, "bool"), kBoolTail);
        if (!ok) std::cout << "    row: " << row.decl << " / " << row.expr << "\n";
        BEZEL_EXPECT_TRUE(ok);
    }
}

// Same sources into `string` (§4.6: no implicit conversion to string).
BEZEL_TEST(untyped_member_source_skips_assignability_4766,
           int_property_into_string_rejected_spec_4_6) {
    for (const auto& row : kRows) {
        bool ok = rejectedWith(repro(row.decl, row.expr, "string"), nullptr);
        if (!ok) std::cout << "    row: " << row.decl << " / " << row.expr << "\n";
        BEZEL_EXPECT_TRUE(ok);
    }
}

// Other §5.1 positions: argument and return value.
BEZEL_TEST(untyped_member_source_skips_assignability_4766,
           argument_and_return_positions_rejected_spec_5_1) {
    auto arg = runAot(R"BZ(
module M;
class C
  public static method Take(flag: bool): int
    return 1;
  end Take;
  public static method Main(): int
    var ss := new SortedSet<int>();
    return Take(ss.Count);
  end Main;
end C;
)BZ");
    BEZEL_EXPECT_FALSE(arg.ran);
    bool sawArg = false;
    for (const auto& d : arg.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kBoolTail) != std::string::npos)
            sawArg = true;
    BEZEL_EXPECT_TRUE(sawArg);

    auto ret = runAot(R"BZ(
module M;
class C
  public static method F(): bool
    var sb := new StringBuilder();
    return sb.Length;
  end F;
  public static method Main(): int
    return 0;
  end Main;
end C;
)BZ");
    BEZEL_EXPECT_FALSE(ret.ran);
    bool sawRet = false;
    for (const auto& d : ret.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("returned value") != std::string::npos &&
            d.message.find(kBoolTail) != std::string::npos)
            sawRet = true;
    BEZEL_EXPECT_TRUE(sawRet);
}

// Accept control: every newly typed read, in int (and int->long widening)
// positions, still compiles, runs, and yields the right values.
BEZEL_TEST(untyped_member_source_skips_assignability_4766,
           int_positions_still_run_control) {
    auto r = runAot(R"BZ(
module M;
class C
  public static method Wide(n: long): long
    return n * 2;
  end Wide;
  public static method Main(): int
    var sb := new StringBuilder();
    sb.Append("hello");
    using var buf := new OwningBuffer<int>(3);
    var fa: FrozenArray<int> := FrozenArray<int>.From([1, 2]);
    var ss := new SortedSet<int>();
    ss.Add(4);
    ss.Add(9);
    var sd := new SortedDictionary<int, int>();
    sd[1] := 10;
    var n: int := sb.Length;
    var m: long := buf.Length;
    Console.WriteLine(n);
    Console.WriteLine(m);
    Console.WriteLine(fa.Length * 10 + 1);
    Console.WriteLine(Wide(ss.Count));
    Console.WriteLine(sd.Count);
    return 0;
  end Main;
end C;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n3\n21\n4\n1\n"));
}
