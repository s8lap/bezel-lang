// Codegen — issue #3483 (§28.12 and §14.2 rule 1, v0.350): the run-time
// `await using` dispatch must select the awaited member by NOMINAL conformance
// and by the interface member's signature, never by the bare name `DisposeAsync`.
//
// §28.12 v0.350, verbatim: "Only an object whose class does not implement
// `IAsyncDisposable` takes the synchronous disposal above." And what
// "implements" means is §14.2's *Interface conformance* rule 1: "T is a class
// that lists I (or any interface extending I) in its inheritance clause, that
// inherits from a class which conforms to I, or that conforms implicitly".
// The member the interface declares is `method DisposeAsync(): Task;`
// (§28.12), matched under §11.2 by name, arity, parameter types and return type.
//
// The first #3483 fix filled the run-time `IAsyncDisposable` dispatch entry
// (declareVtables Pass D) from `findMethodByName(ts, "DisposeAsync")`, a
// NAME-ONLY lookup. Measured against it (review of lane C1-L4, probes in
// /var/tmp/bezel-scratch/r37-rev-l4/q/):
//
//   * w4 — `class Weird` (not `IAsyncDisposable`) with `DisposeAsync(): int`
//     behind an `IDisposable` head: the `int` 7 was awaited as a `Task*`;
//     SEGV in bz_task_is_done. Base printed `body W_DTOR`.
//   * w2 — the same with `DisposeAsync(x: int): int`: abort.
//   * o1 — `class Res: IAsyncDisposable` declaring an overload
//     `DisposeAsync(x: int): int` BEFORE the real one: the overload went into
//     the entry.
//   * w1 — `class Weird` (not `IAsyncDisposable`) with a well-formed
//     `async DisposeAsync(): Task`: awaited, though the class does not
//     implement the interface, so §28.12 says it "takes the synchronous
//     disposal".
//
// Every shape is pinned at three heads the spec names — "through any interface"
// (`IDisposable`), "through `Object`", and "through a base-class static type
// that does not implement `IAsyncDisposable`" — looped 50x, with an owned
// `Cell` field so a skipped cascade shows as a non-zero `cells` (and as an ASan
// leak under `runAotChecked`). The expected line is exact.
//
// On base (before any run-time dispatch) the w-shapes already printed the
// expected synchronous line, so those rows are GUARDS against the name-keyed
// regression, not fail-without rows; the o1 rows and the conformance-route rows
// fail on base (`da=0`, the synchronous path).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr const char* kPreamble = R"(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class Base
  public static bdt: int := 0;
  public constructor() end constructor;
  destructor Base.bdt := Base.bdt + 1; end destructor;
end Base;
)";

// The four shapes. `bases` is the inheritance clause ("" / "Base").
// Every class has the same statics so one `Console.WriteLine` fits all.
std::string classW1(const char* bases) {
    return std::string("public class Weird") + bases + R"(
  public static da: int := 0;
  public static wrong: int := 0;
  public static dt: int := 0;
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Weird.da := Weird.da + 1;
  end DisposeAsync;
  destructor Weird.dt := Weird.dt + 1; end destructor;
end Weird;
)";
}

std::string classW2(const char* bases) {
    return std::string("public class Weird") + bases + R"(
  public static da: int := 0;
  public static wrong: int := 0;
  public static dt: int := 0;
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public method DisposeAsync(x: int): int
    Weird.wrong := Weird.wrong + 1;
    return x + 1;
  end DisposeAsync;
  destructor Weird.dt := Weird.dt + 1; end destructor;
end Weird;
)";
}

std::string classW4(const char* bases) {
    return std::string("public class Weird") + bases + R"(
  public static da: int := 0;
  public static wrong: int := 0;
  public static dt: int := 0;
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public method DisposeAsync(): int
    Weird.wrong := Weird.wrong + 1;
    return 7;
  end DisposeAsync;
  destructor Weird.dt := Weird.dt + 1; end destructor;
end Weird;
)";
}

// CONTROL: a STATIC `DisposeAsync(): Task` is not the instance member; green on
// every arm (the name-only lookup already skipped statics).
std::string classW3(const char* bases) {
    return std::string("public class Weird") + bases + R"(
  public static da: int := 0;
  public static wrong: int := 0;
  public static dt: int := 0;
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public static method DisposeAsync(): Task
    Weird.wrong := Weird.wrong + 1;
    return Task.Delay(0);
  end DisposeAsync;
  destructor Weird.dt := Weird.dt + 1; end destructor;
end Weird;
)";
}

// o1: implements `IAsyncDisposable`; the non-matching overload is declared
// FIRST so a first-by-name pick takes the wrong one.
std::string classO1(const char* bases) {
    return std::string("public class Weird") + bases + R"(
  public static da: int := 0;
  public static wrong: int := 0;
  public static dt: int := 0;
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public method DisposeAsync(x: int): int
    Weird.wrong := Weird.wrong + 1;
    return x + 1;
  end DisposeAsync;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Weird.da := Weird.da + 1;
  end DisposeAsync;
  destructor Weird.dt := Weird.dt + 1; end destructor;
end Weird;
)";
}

constexpr const char* kReport =
    "    Console.WriteLine($\"da={Weird.da} wrong={Weird.wrong} dt={Weird.dt}"
    " bdt={Base.bdt} cells={Cell.n}\");\n";

// "through any interface": an `IDisposable` call head.
std::string idisposableCallHead(const std::string& cls) {
    return std::string(kPreamble) + cls + R"(
public class Program
  public static method Make(): owned IDisposable return new Weird(); end Make;
  public static async method Run(): Task
    await using var d := Program.Make();
  end Run;
  public static async method Main(): Task
    for var i in 0..<50 do await Program.Run(); end for;
)" + kReport + R"(  end Main;
end Program;
)";
}

// "through `Object`".
std::string objectHead(const std::string& cls) {
    return std::string(kPreamble) + cls + R"(
public class Program
  public static async method Run(): Task
    var o: Object := new Weird();
    await using var d: Object := move o;
  end Run;
  public static async method Main(): Task
    for var i in 0..<50 do await Program.Run(); end for;
)" + kReport + R"(  end Main;
end Program;
)";
}

// "through a base-class static type that does not implement
// `IAsyncDisposable`": a `Base` call head (the class lists `Base`).
std::string baseCallHead(const std::string& cls) {
    return std::string(kPreamble) + cls + R"(
public class Program
  public static method Make(): owned Base return new Weird(); end Make;
  public static async method Run(): Task
    await using var d: Base := Program.Make();
  end Run;
  public static async method Main(): Task
    for var i in 0..<50 do await Program.Run(); end for;
)" + kReport + R"(  end Main;
end Program;
)";
}

// A class that does not implement `IAsyncDisposable` takes the synchronous
// disposal: destructor body (dt), cascade (cells), base chain (bdt at a Base
// head); nothing is awaited (da) and no non-matching member runs (wrong).
constexpr const char* kSyncNoBase = "da=0 wrong=0 dt=50 bdt=0 cells=0\n";
constexpr const char* kSyncBase = "da=0 wrong=0 dt=50 bdt=50 cells=0\n";
// A class that implements it: the interface member is awaited, its own
// destructor body is skipped (§28.12 step 1), the cascade and the base chain
// run (step 2), and the non-matching overload never runs.
constexpr const char* kAsyncNoBase = "da=50 wrong=0 dt=0 bdt=0 cells=0\n";
constexpr const char* kAsyncBase = "da=50 wrong=0 dt=0 bdt=50 cells=0\n";

void expectLine(const std::string& program, const char* line) {
    bezel::test::JitResult r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(line));
}

}  // namespace

// ---------------------------------------------------------------------------
// w1: a well-formed `async DisposeAsync(): Task` on a class that does NOT list
// `IAsyncDisposable`. §28.12: "Only an object whose class does not implement
// `IAsyncDisposable` takes the synchronous disposal above" — conformance is
// nominal (§14.2 rule 1), so the member is not awaited.

// narrow #3483 (§28.12 / §14.2 rule 1, v0.350). Name-keyed arm: `da=50 dt=0`.
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w1_nonconforming_dispose_async_idisposable_head_stays_sync_spec_28_12_14_2) {
    expectLine(idisposableCallHead(classW1("")), kSyncNoBase);
}

// narrow #3483 (§28.12 / §14.2 rule 1, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w1_nonconforming_dispose_async_object_head_stays_sync_spec_28_12_14_2) {
    expectLine(objectHead(classW1("")), kSyncNoBase);
}

// narrow #3483 (§28.12 / §14.2 rule 1, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w1_nonconforming_dispose_async_base_head_stays_sync_spec_28_12_14_2) {
    expectLine(baseCallHead(classW1(": Base")), kSyncBase);
}

// ---------------------------------------------------------------------------
// w2: `DisposeAsync(x: int): int` on a non-conforming class. Name-keyed arm:
// abort (the `int` return awaited as a task).

// narrow #3483 (§28.12 / §11.2 arity, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w2_wrong_arity_dispose_async_idisposable_head_stays_sync_spec_28_12) {
    expectLine(idisposableCallHead(classW2("")), kSyncNoBase);
}

// narrow #3483 (§28.12 / §11.2 arity, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w2_wrong_arity_dispose_async_object_head_stays_sync_spec_28_12) {
    expectLine(objectHead(classW2("")), kSyncNoBase);
}

// narrow #3483 (§28.12 / §11.2 arity, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w2_wrong_arity_dispose_async_base_head_stays_sync_spec_28_12) {
    expectLine(baseCallHead(classW2(": Base")), kSyncBase);
}

// ---------------------------------------------------------------------------
// w4: `DisposeAsync(): int` on a non-conforming class. Name-keyed arm: SEGV in
// bz_task_is_done (7 dereferenced as a task).

// narrow #3483 (§28.12 / §11.2 return type, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w4_int_returning_dispose_async_idisposable_head_stays_sync_spec_28_12) {
    expectLine(idisposableCallHead(classW4("")), kSyncNoBase);
}

// narrow #3483 (§28.12 / §11.2 return type, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w4_int_returning_dispose_async_object_head_stays_sync_spec_28_12) {
    expectLine(objectHead(classW4("")), kSyncNoBase);
}

// narrow #3483 (§28.12 / §11.2 return type, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w4_int_returning_dispose_async_base_head_stays_sync_spec_28_12) {
    expectLine(baseCallHead(classW4(": Base")), kSyncBase);
}

// ---------------------------------------------------------------------------
// o1: a conforming class whose first-declared `DisposeAsync` is a non-matching
// overload. The interface member — zero parameters, returns `Task` — is the
// one awaited (§11.2 matching); the overload never runs. Base (no run-time
// dispatch at all): `da=0 wrong=0 dt=50`.

// narrow #3483 (§28.12 / §11.2, v0.350). Name-keyed arm: abort.
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           o1_overload_first_idisposable_head_awaits_interface_member_spec_28_12_3483) {
    expectLine(idisposableCallHead(classO1(": IAsyncDisposable")), kAsyncNoBase);
}

// narrow #3483 (§28.12 / §11.2, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           o1_overload_first_object_head_awaits_interface_member_spec_28_12_3483) {
    expectLine(objectHead(classO1(": IAsyncDisposable")), kAsyncNoBase);
}

// narrow #3483 (§28.12 / §11.2, v0.350).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           o1_overload_first_base_head_awaits_interface_member_spec_28_12_3483) {
    expectLine(baseCallHead(classO1(": Base, IAsyncDisposable")), kAsyncBase);
}

// ---------------------------------------------------------------------------
// CONTROL w3: a static `DisposeAsync(): Task` is not the instance member.
// Green on base, on the name-keyed arm and here.

// §28.12 / §11.2 (v0.350) control.
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           w3_static_dispose_async_idisposable_head_stays_sync_control_spec_28_12) {
    expectLine(idisposableCallHead(classW3("")), kSyncNoBase);
}

// ---------------------------------------------------------------------------
// §14.2 rule 1's conformance ROUTES, through an `IDisposable` call head:
// "lists I (or any interface extending I) in its inheritance clause".

// narrow #3483 (§14.2 rule 1 "any interface extending I" / §28.12, v0.350):
// the class lists a user interface that extends `IAsyncDisposable`. Base:
// `da=0 dt=50` (synchronous).
BEZEL_TEST(await_using_runtime_dispose_async_nominal,
           conformance_through_extending_interface_awaits_spec_14_2_rule_1_3483) {
    constexpr const char* kProgram = R"(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public interface IMine: IAsyncDisposable
  method Extra(): int;
end IMine;
public class Res: IMine
  public static da: int := 0;
  public static dt: int := 0;
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public method Extra(): int return 1; end Extra;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Res.da := Res.da + 1;
  end DisposeAsync;
  destructor Res.dt := Res.dt + 1; end destructor;
end Res;
public class Program
  public static method Make(): owned IDisposable return new Res(); end Make;
  public static async method Run(): Task
    await using var d := Program.Make();
  end Run;
  public static async method Main(): Task
    for var i in 0..<50 do await Program.Run(); end for;
    Console.WriteLine($"da={Res.da} dt={Res.dt} cells={Cell.n}");
  end Main;
end Program;
)";
    expectLine(kProgram, "da=50 dt=0 cells=0\n");
}
