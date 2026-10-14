// fix #6286 — spec §30.14 (v0.338): a receiver is a capture of the entry.
//
// "A capturing lambda or an instance method reference is rejected — captures
// and receivers are borrows of the origin domain." The capture walk
// (isCaptureFreeEntry / exprCaptures) saw only locals and parameters, so an
// entry lambda in an instance method that reads an instance field (`_v`), calls
// an instance method (`Get()`), reads an instance property (`P`) or names the
// receiver (`this`, `base`) compiled, although the receiver is a borrow of the
// origin domain. The walk now treats an instance member read by its bare name,
// and the receiver keywords, as a capture at the callers whose admission rule
// is the Domain.Spawn one: Domain.Spawn (§30.14), SharedEventSource Subscribe
// (§6.5 / §28.14: "the same admission as a Domain.Spawn entry") and
// `static event +=` (§28.14: the same admission). The plain extern callback
// (§32.8) is not changed here: its receiver rule is the subject of a
// spec-question.
//
// Static members (bare or qualified), an entry's own locals and loop variables
// remain non-captures in an instance method.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool rejectedBy(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kSpawnNeedle = "Domain.Spawn entry must be capture-free";
const char* kSubscribeNeedle =
    "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be "
    "capture-free";
const char* kStaticEventNeedle =
    "handler subscribed to a `static event` must be capture-free";

// An instance method `Run` that spawns an entry whose body is `body`. The
// class has an instance field `_v`, an instance property `P`, an instance
// method `Get`, and static members `Limit` and `Twice`.
std::string instanceSpawnProgram(const std::string& body) {
    return "module T; "
           "public class C "
           "  private _v: int := 3; "
           "  public property P: int => 7; "
           "  public static readonly Limit: int := 2; "
           "  public static method Twice(x: int): int return x * 2; end Twice; "
           "  public method Get(): int return 4; end Get; "
           "  public method Run(): int "
           "    using var u := Domain.Spawn(lambda() => do " +
           body +
           " end); "
           "    u.Wait(); "
           "    return 0; "
           "  end Run; "
           "  public static method Main(args: Array<string>): int "
           "    var c := new C(); "
           "    return c.Run(); "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---- Domain.Spawn in an instance method: receiver captures -----------------

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_instance_field_read_rejected_spec_30_14) {
    auto r = runAot(instanceSpawnProgram("Console.WriteLine(_v);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_instance_method_call_rejected_spec_30_14) {
    auto r = runAot(instanceSpawnProgram("Console.WriteLine(Get());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_instance_property_read_rejected_spec_30_14) {
    auto r = runAot(instanceSpawnProgram("Console.WriteLine(P);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_explicit_this_field_rejected_spec_30_14) {
    auto r = runAot(instanceSpawnProgram("Console.WriteLine(this._v);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_explicit_this_method_call_rejected_spec_30_14) {
    auto r = runAot(instanceSpawnProgram("Console.WriteLine(this.Get());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_instance_field_in_for_body_rejected_spec_30_14) {
    auto r = runAot(instanceSpawnProgram(
        "for var i in 1..2 do Console.WriteLine(_v); end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

// ---- Domain.Spawn in an instance method: valid shapes stay accepted --------

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_static_member_bare_in_instance_method_accepted_spec_30_14) {
    auto r = runAot(instanceSpawnProgram(
        "for var i in 1..Limit do Console.WriteLine(Twice(i)); end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_static_member_qualified_in_instance_method_accepted_spec_30_14) {
    auto r = runAot(instanceSpawnProgram(
        "for var i in 1..C.Limit do Console.WriteLine(C.Twice(i)); end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           spawn_own_locals_in_instance_method_accepted_spec_30_14) {
    auto r = runAot(instanceSpawnProgram(
        "var k := 1; Console.WriteLine(k); "
        "for var i in 1..2 do var m := i; Console.WriteLine(m); end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

// ---- Subscribe (§6.5) and `static event +=` (§28.14) in an instance method --

BEZEL_TEST(spawn_implicit_this_6286,
           subscribe_instance_field_in_instance_method_rejected_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "public class C "
        "  private _v: int := 3; "
        "  public method Run(): int "
        "    var sub := Buss.Feed.Subscribe(lambda(x: int) => do "
        "Console.WriteLine(_v); end); "
        "    Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "  end Run; "
        "  public static method Main(): int var c := new C(); return c.Run(); end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSubscribeNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           subscribe_static_member_in_instance_method_accepted_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "public class C "
        "  public static readonly Limit: int := 2; "
        "  public method Run(): int "
        "    var sub := Buss.Feed.Subscribe(lambda(x: int) => do "
        "var k := x + Limit; Console.WriteLine(k); end); "
        "    Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "  end Run; "
        "  public static method Main(): int var c := new C(); return c.Run(); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSubscribeNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           static_event_instance_field_in_instance_method_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  private _v: int := 3; "
        "  public static event Ev: int; "
        "  public method Run(): int "
        "    Ev += lambda(x: int) => do Console.WriteLine(_v); end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int var c := new C(); return c.Run(); end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kStaticEventNeedle));
}

BEZEL_TEST(spawn_implicit_this_6286,
           static_event_static_member_in_instance_method_accepted_spec_28_14) {
    auto r = runAot(
        "module M; "
        "public class C "
        "  public static readonly Limit: int := 2; "
        "  public static event Ev: int; "
        "  public method Run(): int "
        "    Ev += lambda(x: int) => do Console.WriteLine(x + Limit); end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int var c := new C(); return c.Run(); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kStaticEventNeedle));
}
