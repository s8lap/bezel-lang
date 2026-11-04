// Regression tests for #8089: a record's block-form property setter body is a
// member body like any other, so a store landing in the receiver's own storage
// is rejected there. Spec §9.4 (v0.353): "a record may still declare a
// block-form property or indexer ... — including a setter in the mediating
// shape above, every store landing beyond a class- or interface-typed hop —
// but a *setter body* whose store lands in the receiver's own storage is
// rejected like any other member body." Conformance cases s9_4_0013 (reject)
// and s9_4_0014 (legal twin).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}
constexpr const char* kStore = "to the receiver's own storage";
const char* kMain =
    "public class Program "
    "  public static method Main() "
    "    Console.WriteLine(1); "
    "  end Main; "
    "end Program;";
}  // namespace

// s9_4_0013: the issue's repro.
BEZEL_TEST(record_setter_receiver_store_8089,
           setter_store_into_own_storage_rejected_spec_9_4) {
    auto r = runAot(
        std::string("module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public property Doubled: int "
        "    get => Raw * 2; "
        "    set => Raw := value / 2; "
        "  end Doubled; "
        "end Gauge; ") + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kStore));
}

// Statement-block setter body, explicit `this.` spelling.
BEZEL_TEST(record_setter_receiver_store_8089,
           block_setter_this_store_rejected_spec_9_4) {
    auto r = runAot(
        std::string("module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public property Doubled: int "
        "    get => Raw * 2; "
        "    set => do this.Raw := value / 2; end; "
        "  end Doubled; "
        "end Gauge; ") + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kStore));
}

// A getter that only reads stays accepted.
BEZEL_TEST(record_setter_receiver_store_8089,
           getter_read_only_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public record Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public property Doubled: int "
        "    get => Raw * 2; "
        "  end Doubled; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(new Gauge(4).Doubled); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\n"));
}

// s9_4_0014: the mediating shape (store beyond a class-typed hop) is legal.
BEZEL_TEST(record_setter_receiver_store_8089,
           setter_store_beyond_class_hop_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public class Cell "
        "  public property Value: int; "
        "  public constructor(value: int) "
        "    Value := value; "
        "  end constructor; "
        "end Cell; "
        "public record View "
        "  public readonly property Target: Cell; "
        "  public constructor(target: Cell) "
        "    Target := target; "
        "  end constructor; "
        "  public property Level: int "
        "    get => Target.Value; "
        "    set => Target.Value := value; "
        "  end Level; "
        "end View; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new Cell(0); "
        "    var v := new View(c); "
        "    v.Level := 7; "
        "    Console.WriteLine(c.Value); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// A CLASS setter doing the same store is legal (§9.4 governs value kinds).
BEZEL_TEST(record_setter_receiver_store_8089,
           class_setter_own_storage_store_accepted_spec_9_4) {
    auto r = runAot(
        "module Conf; "
        "public class Gauge "
        "  public property Raw: int; "
        "  public constructor(raw: int) "
        "    Raw := raw; "
        "  end constructor; "
        "  public property Doubled: int "
        "    get => Raw * 2; "
        "    set => Raw := value / 2; "
        "  end Doubled; "
        "end Gauge; "
        "public class Program "
        "  public static method Main() "
        "    using var g := new Gauge(1); "
        "    g.Doubled := 10; "
        "    Console.WriteLine(g.Raw); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}
