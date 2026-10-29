// Codegen — issue #3483 (§28.12, v0.350): an `await using` head whose static
// type names no `DisposeAsync` the compiler can resolve — an interface, `Object`,
// a call returning `owned IAsyncDisposable` — took the SYNCHRONOUS disposal, so
//
//     public static method Make(): owned IAsyncDisposable return new AsyncRes(); end Make;
//     ...
//     await using var d := Program.Make();
//
// ran `AsyncRes`'s destructor body (`body DTOR`) and never awaited its
// `DisposeAsync`. §28.12 v0.350 states, verbatim:
//
//   "The `DisposeAsync` awaited is the run-time object's. `await using`
//    selects the logical-disposal path by the class of the object the binding
//    holds ... never by the binding's static type."
//
// and names the static types this covers: "through `Object`, and through any
// interface". "Only an object whose class does not implement
// `IAsyncDisposable` takes the synchronous disposal above." Step 1: under
// `await using` the destructor body is not run, because `DisposeAsync`
// supersedes it.
//
// The mechanism (the #3493 owner ruling of 2026-10-27, option 2: a
// `DisposeAsync` dispatch-table slot, the run-time object decides): every
// non-generic class that declares a `DisposeAsync` carries an
// `IAsyncDisposable` entry in its itable map, `[DisposeAsync, DisposeReclaim]`,
// and an `await using` head that resolves no `DisposeAsync` statically looks
// the entry up on the run-time object. Found: the run-time class's
// `DisposeAsync` is awaited and its reclamation runs. Absent: the synchronous
// disposal, as before.
//
// Every row asserts the PRINTED ORDER (or a counter tuple), never only the
// absence of a leak; `runAotChecked` adds the generated-code ASan leg.
//
// ⚠ NARROW. A GENERIC class (`Gen<T>: IAsyncDisposable`) behind an interface
// head still takes the synchronous disposal: the per-class reclamation function
// the entry points at is built for non-generic classes only. Not pinned here.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The issue's literal repro.
constexpr const char* kLiteral = R"(
module M3483;
public class AsyncRes: IAsyncDisposable
  public constructor() end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;
  destructor Console.WriteLine("DTOR"); end destructor;
end AsyncRes;
public class Program
  public static method Make(): owned IAsyncDisposable
    return new AsyncRes();
  end Make;
  public static async method Main(): Task
    await using var d := Program.Make();
    Console.WriteLine("body");
  end Main;
end Program;
)";

// The literal repro, looped 60x, with an owned field on the async class, so a
// skipped reclamation would show as a stranded `Cell` (and an ASan leak).
constexpr const char* kLooped = R"(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class AsyncRes: IAsyncDisposable
  public static da: int := 0;
  public static dt: int := 0;
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    AsyncRes.da := AsyncRes.da + 1;
  end DisposeAsync;
  destructor AsyncRes.dt := AsyncRes.dt + 1; end destructor;
end AsyncRes;
public class Program
  public static method Make(): owned IAsyncDisposable return new AsyncRes(); end Make;
  public static async method Run(): Task
    await using var d := Program.Make();
  end Run;
  public static async method Main(): Task
    for var i in 0..<60 do await Program.Run(); end for;
    Console.WriteLine($"da={AsyncRes.da} dt={AsyncRes.dt} cells={Cell.n}");
  end Main;
end Program;
)";

// The shared async class for the head-shape siblings.
#define BZ_ASYNC_RES                                                          \
    "public class AsyncRes: IAsyncDisposable\n"                               \
    "  public constructor() end constructor;\n"                               \
    "  public async method DisposeAsync(): Task\n"                            \
    "    await Task.Delay(0);\n"                                              \
    "    Console.WriteLine(\"DA\");\n"                                        \
    "  end DisposeAsync;\n"                                                   \
    "  destructor Console.WriteLine(\"DTOR\"); end destructor;\n"             \
    "end AsyncRes;\n"                                                         \
    "public class Plain\n"                                                    \
    "  public constructor() end constructor;\n"                               \
    "  destructor Console.WriteLine(\"PLAIN_DTOR\"); end destructor;\n"       \
    "end Plain;\n"

// "through any interface": an explicitly typed `IAsyncDisposable` head.
constexpr const char* kTypedInterfaceHead =
    "module M;\n" BZ_ASYNC_RES
    "public class Program\n"
    "  public static method Make(): owned IAsyncDisposable return new AsyncRes(); end Make;\n"
    "  public static async method Main(): Task\n"
    "    await using var d: IAsyncDisposable := Program.Make();\n"
    "    Console.WriteLine(\"body\");\n"
    "  end Main;\n"
    "end Program;\n";

// "through any interface": an `IDisposable` head whose object is async.
constexpr const char* kIDisposableHead =
    "module M;\n" BZ_ASYNC_RES
    "public class Program\n"
    "  public static method Make(): owned IDisposable return new AsyncRes(); end Make;\n"
    "  public static async method Main(): Task\n"
    "    await using var d := Program.Make();\n"
    "    Console.WriteLine(\"body\");\n"
    "  end Main;\n"
    "end Program;\n";

// "through any interface": a user interface the async class also implements.
constexpr const char* kUserInterfaceHead = R"(
module M;
public interface IThing
  method Name(): int;
end IThing;
public class AsyncRes: IThing, IAsyncDisposable
  public constructor() end constructor;
  public method Name(): int return 1; end Name;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;
  destructor Console.WriteLine("DTOR"); end destructor;
end AsyncRes;
public class Program
  public static method Make(): owned IThing return new AsyncRes(); end Make;
  public static async method Main(): Task
    await using var d := Program.Make();
    Console.WriteLine("body");
  end Main;
end Program;
)";

// "through `Object`".
constexpr const char* kObjectHead =
    "module M;\n" BZ_ASYNC_RES
    "public class Program\n"
    "  public static async method Main(): Task\n"
    "    var o: Object := new AsyncRes();\n"
    "    await using var d: Object := move o;\n"
    "    Console.WriteLine(\"body\");\n"
    "  end Main;\n"
    "end Program;\n";

// A nullable head: the non-null one awaits `DisposeAsync`, the null one is
// skipped (§28.10: a null binding is not disposed).
constexpr const char* kNullableHead =
    "module M;\n" BZ_ASYNC_RES
    "public class Program\n"
    "  public static method Make(b: bool): owned IAsyncDisposable?\n"
    "    if b then return new AsyncRes(); end;\n"
    "    return null;\n"
    "  end Make;\n"
    "  public static async method Main(): Task\n"
    "    await using var d := Program.Make(true);\n"
    "    await using var e := Program.Make(false);\n"
    "    Console.WriteLine(\"body\");\n"
    "  end Main;\n"
    "end Program;\n";

// The run-time object decides, per binding: one `IDisposable` call site
// returning an async object and a plain one. Reverse declaration order: `b`
// (Plain, synchronous: "Only an object whose class does not implement
// `IAsyncDisposable` takes the synchronous disposal") first, then `a` (DA).
constexpr const char* kMixedRunTimeClasses =
    "module M;\n" BZ_ASYNC_RES
    "public class Program\n"
    "  public static method Make(b: bool): owned IDisposable\n"
    "    if b then return new AsyncRes(); end;\n"
    "    return new Plain();\n"
    "  end Make;\n"
    "  public static async method Main(): Task\n"
    "    await using var a := Program.Make(true);\n"
    "    await using var b := Program.Make(false);\n"
    "    Console.WriteLine(\"body\");\n"
    "  end Main;\n"
    "end Program;\n";

// A generic method body over `T: IAsyncDisposable`, instantiated from a class
// head and from an interface head.
constexpr const char* kGenericParamHead =
    "module M;\n" BZ_ASYNC_RES
    "public class Program\n"
    "  public static method Make(): owned IAsyncDisposable return new AsyncRes(); end Make;\n"
    "  public static async method Use<T>(x: owned T): Task where T: IAsyncDisposable\n"
    "    await using var d := move x;\n"
    "    Console.WriteLine(\"body\");\n"
    "  end Use;\n"
    "  public static async method Main(): Task\n"
    "    await Program.Use(new AsyncRes());\n"
    "    await Program.Use(Program.Make());\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL: an interface head over a class that does not implement
// `IAsyncDisposable` keeps the synchronous disposal — the destructor runs.
constexpr const char* kPlainControl =
    "module M;\n" BZ_ASYNC_RES
    "public class Program\n"
    "  public static method Make(): owned IDisposable return new Plain(); end Make;\n"
    "  public static async method Main(): Task\n"
    "    await using var d := Program.Make();\n"
    "    Console.WriteLine(\"body\");\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// fix #3483 (§28.12, v0.350): the issue's literal repro. Before: `body\nDTOR\n`.
BEZEL_TEST(await_using_runtime_dispose_async,
           interface_call_head_awaits_runtime_dispose_async_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kLiteral);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\n");
}

// fix #3483 (§28.12, v0.350): 60 iterations; every `DisposeAsync` awaited, no
// destructor body, every owned `Cell` reclaimed. Before: `da=0 dt=60 cells=0`.
BEZEL_TEST(await_using_runtime_dispose_async,
           interface_call_head_looped_awaits_and_reclaims_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kLooped);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "da=60 dt=0 cells=0\n");
}

// fix #3483 (§28.12, v0.350): "through any interface" — typed head.
BEZEL_TEST(await_using_runtime_dispose_async,
           typed_iasyncdisposable_head_awaits_dispose_async_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kTypedInterfaceHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\n");
}

// fix #3483 (§28.12, v0.350): "through any interface" — `IDisposable` head.
BEZEL_TEST(await_using_runtime_dispose_async,
           idisposable_head_over_async_object_awaits_dispose_async_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kIDisposableHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\n");
}

// fix #3483 (§28.12, v0.350): "through any interface" — a user interface.
BEZEL_TEST(await_using_runtime_dispose_async,
           user_interface_head_awaits_dispose_async_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kUserInterfaceHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\n");
}

// fix #3483 (§28.12, v0.350): "through `Object`".
BEZEL_TEST(await_using_runtime_dispose_async,
           object_head_awaits_dispose_async_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kObjectHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\n");
}

// fix #3483 (§28.12, v0.350): nullable head — one DA, the null head skipped.
BEZEL_TEST(await_using_runtime_dispose_async,
           nullable_interface_head_awaits_dispose_async_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kNullableHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\n");
}

// fix #3483 (§28.12, v0.350): one declared type, two run-time classes; each
// binding takes its own object's path. Before: `body\nPLAIN_DTOR\nDTOR\n`.
BEZEL_TEST(await_using_runtime_dispose_async,
           runtime_class_selects_path_per_binding_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kMixedRunTimeClasses);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nPLAIN_DTOR\nDA\n");
}

// fix #3483 (§28.12, v0.350): generic `T: IAsyncDisposable` body, both
// instantiations await `DisposeAsync`.
BEZEL_TEST(await_using_runtime_dispose_async,
           generic_param_head_awaits_dispose_async_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kGenericParamHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\nbody\nDA\n");
}

// CONTROL (§28.12, v0.350): "Only an object whose class does not implement
// `IAsyncDisposable` takes the synchronous disposal above." Green on both arms.
BEZEL_TEST(await_using_runtime_dispose_async,
           interface_head_over_plain_class_stays_synchronous_control_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kPlainControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nPLAIN_DTOR\n");
}
