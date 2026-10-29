// Codegen — issue #7531 (§28.12 and §14.2 rule 1, v0.350): the STATIC
// `await using` arm (`registerUsingDisposal`, a `new C()` right-hand side that
// settles the class) selected the awaited member by the bare name
// `DisposeAsync` — `findMethodByName(cls, "DisposeAsync")` — checking neither
// that the class implements `IAsyncDisposable` nor the member's signature.
// Measured on the base (review of lane C1-L4, probes in
// /var/tmp/bezel-scratch/r37-rev-l4/q/, the `static_*` and `o1s` files):
//
//   * `class Weird` (not `IAsyncDisposable`) with `DisposeAsync(): int`: the
//     `int` 7 was awaited as a `Task*` — ASan SEGV at address 0x17.
//   * the same with `DisposeAsync(x: int): int`: SEGV (garbage task pointer).
//   * `class Res: IAsyncDisposable` declaring `DisposeAsync(x: int): int`
//     BEFORE the real `async DisposeAsync(): Task`: the first-by-name overload
//     was awaited — SEGV.
//   * `class Weird` (not `IAsyncDisposable`) with a well-formed
//     `async DisposeAsync(): Task`: awaited (`body`, no `W_DTOR`), though
//     §28.12 says "Only an object whose class does not implement
//     `IAsyncDisposable` takes the synchronous disposal above", and what
//     "implements" means is §14.2 rule 1: "T is a class that lists I (or any
//     interface extending I) in its inheritance clause, that inherits from a
//     class which conforms to I, or that conforms implicitly" — a nominal
//     relation a same-named member does not create (§8.7: "`DisposeAsync()` is
//     a member only of a type that declares or inherits it — `IAsyncDisposable`,
//     the interfaces extending it, a class implementing it").
//
// The fix keys the static arm, the reclaim-function population and the
// run-time dispatch entry on ONE predicate (`asyncDisposableDisposeAsyncOf`):
// nominal conformance per §14.2 rule 1 and the interface member's signature
// per §11.2 ("a return type that is exactly equal", "the same arity").
//
// Each row loops 50x with counters and an owned `Cell` (a skipped cascade shows
// as `cells<>0` and as an ASan leak under `runAotChecked`). Exact stdout.

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
)";

constexpr const char* kProgram = R"(
public class Program
  public static async method Run(): Task
    await using var d := new Weird();
  end Run;
  public static async method Main(): Task
    for var i in 0..<50 do await Program.Run(); end for;
    Console.WriteLine($"da={Weird.da} wrong={Weird.wrong} dt={Weird.dt} cells={Cell.n}");
  end Main;
end Program;
)";

std::string program(const char* cls) {
    return std::string(kPreamble) + cls + kProgram;
}

// A class that does not implement `IAsyncDisposable`: the synchronous disposal
// (destructor body, cascade); nothing awaited, no non-matching member run.
constexpr const char* kSync = "da=0 wrong=0 dt=50 cells=0\n";
// A class that implements it: the interface member awaited, its destructor
// body skipped (§28.12 step 1), the cascade run (step 2), the overload idle.
constexpr const char* kAsync = "da=50 wrong=0 dt=0 cells=0\n";

void expectLine(const std::string& src, const char* line) {
    bezel::test::JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(line));
}

}  // namespace

// fix #7531 (§28.12 / §14.2 rule 1, v0.350): a well-formed
// `async DisposeAsync(): Task` on a class that does NOT list
// `IAsyncDisposable` is not the interface member; the object "takes the
// synchronous disposal". Before: `da=50 wrong=0 dt=0` (awaited).
BEZEL_TEST(await_using_static_head_dispose_async_conformance,
           static_head_nonconforming_dispose_async_stays_sync_spec_28_12_14_2_7531) {
    expectLine(program(R"(
public class Weird
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
)"), kSync);
}

// fix #7531 (§28.12 / §11.2 arity, v0.350): `DisposeAsync(x: int): int` on a
// non-conforming class. Before: SEGV (its result awaited as a task).
BEZEL_TEST(await_using_static_head_dispose_async_conformance,
           static_head_wrong_arity_dispose_async_stays_sync_spec_28_12_7531) {
    expectLine(program(R"(
public class Weird
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
)"), kSync);
}

// fix #7531 (§28.12 / §11.2 return type, v0.350): `DisposeAsync(): int` on a
// non-conforming class. Before: SEGV at 0x17 (the `int` 7 as a task pointer).
BEZEL_TEST(await_using_static_head_dispose_async_conformance,
           static_head_int_returning_dispose_async_stays_sync_spec_28_12_7531) {
    expectLine(program(R"(
public class Weird
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
)"), kSync);
}

// fix #7531 (§28.12 / §11.2, v0.350): a conforming class whose first-declared
// `DisposeAsync` is a non-matching overload; the interface member is awaited.
// Before: SEGV (the overload's `int` awaited).
BEZEL_TEST(await_using_static_head_dispose_async_conformance,
           static_head_overload_first_awaits_interface_member_spec_28_12_7531) {
    expectLine(program(R"(
public class Weird: IAsyncDisposable
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
)"), kAsync);
}

// §28.12 / §11.2 (v0.350) CONTROL: a static `DisposeAsync(): Task` is not the
// instance member. Green before and after (statics were already skipped).
BEZEL_TEST(await_using_static_head_dispose_async_conformance,
           static_head_static_dispose_async_stays_sync_control_spec_28_12) {
    expectLine(program(R"(
public class Weird
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
)"), kSync);
}

// fix #7531 (§28.12 / §14.2 rule 1 "inherits from a class which conforms to
// I", v0.350): a conforming BASE's interface member, reached from a derived
// class's static head. Green before (the name walk reached the base too);
// pins that the predicate's conformance walk includes the base chain.
BEZEL_TEST(await_using_static_head_dispose_async_conformance,
           static_head_derived_of_conforming_base_awaits_base_member_spec_14_2_rule_1) {
    constexpr const char* kSrc = R"(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class Base: IAsyncDisposable
  public static da: int := 0;
  public static bdt: int := 0;
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Base.da := Base.da + 1;
  end DisposeAsync;
  destructor Base.bdt := Base.bdt + 1; end destructor;
end Base;
public class Der: Base
  public static ddt: int := 0;
  public constructor() end constructor;
  destructor Der.ddt := Der.ddt + 1; end destructor;
end Der;
public class Program
  public static async method Run(): Task
    await using var d := new Der();
  end Run;
  public static async method Main(): Task
    for var i in 0..<50 do await Program.Run(); end for;
    Console.WriteLine($"da={Base.da} cells={Cell.n}");
  end Main;
end Program;
)";
    bezel::test::JitResult r = runAotChecked(kSrc);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Deliberately NOT pinned: which level's destructor body is skipped.
    // §28.12 step 2's per-level rule ("at each level of the reclamation chain
    // the destructor body is skipped if that level declares or overrides a
    // non-`abstract` `DisposeAsync`") is issue #3515's open half; the chain
    // today skips the declared class's body and runs the base's.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("da=50 cells=0\n"));
}
