// Spec-derived regression test for issue #7498 — the REMAINING built-in
// constructor `lowerNew` arms still bound a REORDERED named argument
// POSITIONALLY (a silent wrong answer), after #7455 fixed only the §2.5
// date/time arms.
//
// spec §22.5 "Optional parameters and named arguments" (v0.350): "a named
// argument binds the parameter it NAMES, in any order after the positional
// ones, and each supplied argument binds exactly one parameter" — the same
// sentence governs a call, a `new` expression and a record construction.
// stdlib §6.3 `Semaphore(initialCount, maxCount)`, §4.14
// `OwningBuffer<T>(length, init)` and `ArraySpan<T>(Source, Start, Length)`,
// §7.2 `FileStream(path, mode, access)`, §2.15
// `JsonDeserializer(input, options)` and
// `BinaryDeserializer(input, ignoreUnknownFields)` (v0.350).
//
// MEASURED on 7358f89f (build/bezelc, real AOT compile + native run):
//
//   new Semaphore(maxCount := 5, initialCount := 2)
//       -> System.ArgumentException at run time (values swapped)
//   new OwningBuffer<int>(init := ..., length := 3)
//       -> compile failure "Load operand must be a pointer"
//   new FileStream(access := ..., mode := ..., path := ...)
//       -> refused: "produced no value ... matches no documented constructor"
//   new ArraySpan<int>(Length := 2, Start := 1, Source := arr)
//       -> compile failure "Trunc only operates on integer"
//
// The same pre-pass also EVALUATED each reordered argument TWICE
// (`new Semaphore(maxCount := F(), initialCount := G())` ran F G G F): a
// side effect ran twice and a reordered lambda leaked its first closure cell.
// Every row below is a VALUE oracle (it prints what was built), so a
// positional mis-bind cannot pass.

#include <string>

#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& imports, const std::string& methods,
                 const std::string& body) {
    return "module M; import System; " + imports +
           " class C " + methods +
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// stdlib §6.3: `initialCount` permits at construction, `maxCount` the
// ceiling.  maxCount 5 / initialCount 2 -> two acquires succeed, the third
// finds the pool empty.  Pre-fix: ArgumentException (initial 5 > max 2).
BEZEL_TEST(builtin_ctor_named_arg_7498,
           semaphore_reordered_named_args_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "import System.Threading;", "",
            "    using var s := new Semaphore(maxCount := 5, "
            "                                 initialCount := 2); "
            "    using var t1 := s.Acquire(); "
            "    using var t2 := s.Acquire(); "
            "    using var t3 := s.TryAcquire(TimeSpan.FromMilliseconds(1.0)); "
            "    if t3 = null then Console.WriteLine(\"exhausted\"); "
            "    else Console.WriteLine(\"extra permit\"); end; ")),
        std::string("exhausted"));
}

// The in-order named spelling and the positional one keep working.
BEZEL_TEST(builtin_ctor_named_arg_7498,
           semaphore_in_order_named_and_positional_args_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "import System.Threading;", "",
            "    using var a := new Semaphore(initialCount := 1, "
            "                                 maxCount := 5); "
            "    using var b := new Semaphore(1, 5); "
            "    using var ta := a.Acquire(); "
            "    using var tb := b.Acquire(); "
            "    using var ra := a.TryAcquire(TimeSpan.FromMilliseconds(1.0)); "
            "    using var rb := b.TryAcquire(TimeSpan.FromMilliseconds(1.0)); "
            "    Console.WriteLine((ra = null) and (rb = null)); ")),
        std::string("True"));
}

// §22.5 evaluation order: written order (M, I), each argument ONCE.
// Pre-fix: "MIIM" (the pre-pass, then the arm's own lowering).
BEZEL_TEST(builtin_ctor_named_arg_7498,
           reordered_builtin_ctor_args_are_evaluated_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "import System.Threading;",
            "  public static method T(sb: StringBuilder, s: string, v: int)"
            "      : int "
            "    sb.Append(s); "
            "    return v; "
            "  end T; ",
            "    using var sb := new StringBuilder(); "
            "    using var s := new Semaphore(maxCount := T(sb, \"M\", 5), "
            "                                 initialCount := T(sb, \"I\", 2)); "
            "    Console.WriteLine(sb.ToString()); ")),
        std::string("MI"));
}

// stdlib §4.14: `length` is the extent, `init` fills slot i.  Pre-fix: the
// swapped values failed to compile.
BEZEL_TEST(builtin_ctor_named_arg_7498,
           owningbuffer_reordered_named_args_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "", "",
            "    using var b := new OwningBuffer<int>("
            "        init := lambda(i) => i * 10, length := 3); "
            "    Console.WriteLine($\"{b.Length} {b[0]} {b[1]} {b[2]}\"); ")),
        std::string("3 0 10 20"));
}

// stdlib §7.2: `path`, `mode`, `access` bind by name.  The byte reaches the
// file (Create + Write), so a swapped mode/access cannot read back 1 byte.
BEZEL_TEST(builtin_ctor_named_arg_7498,
           filestream_reordered_named_args_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(bezel::test::spliceDataPaths(
            prog("import System.IO;",
                 "  private static method W(path: string) "
                 "    using var fs := new FileStream("
                 "        access := FileAccess.Write, mode := FileMode.Create, "
                 "        path := path); "
                 "    using var b := new Array<byte>(); "
                 "    b.Add(byte(7)); "
                 "    fs.Write(b, 0, 1); "
                 "  end W; ",
                 "    W(\"@DATA@/bzt7498_fs.bin\"); "
                 "    using var back := File.ReadAllBytes("
                 "        \"@DATA@/bzt7498_fs.bin\"); "
                 "    File.Delete(\"@DATA@/bzt7498_fs.bin\"); "
                 "    Console.WriteLine($\"{back.Count} {back[0]}\"); "),
            "bzt7498")),
        std::string("1 7"));
}

// Two-argument FileStream, reordered: `access` keeps its declared default
// (ReadWrite, stdlib §7.2).
BEZEL_TEST(builtin_ctor_named_arg_7498,
           filestream_two_arg_reordered_named_args_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(bezel::test::spliceDataPaths(
            prog("import System.IO;",
                 "  private static method W(path: string) "
                 "    using var fs := new FileStream("
                 "        mode := FileMode.Create, path := path); "
                 "    using var b := new Array<byte>(); "
                 "    b.Add(byte(9)); "
                 "    fs.Write(b, 0, 1); "
                 "  end W; ",
                 "    W(\"@DATA@/bzt7498_fs2.bin\"); "
                 "    using var back := File.ReadAllBytes("
                 "        \"@DATA@/bzt7498_fs2.bin\"); "
                 "    File.Delete(\"@DATA@/bzt7498_fs2.bin\"); "
                 "    Console.WriteLine($\"{back.Count} {back[0]}\"); "),
            "bzt7498")),
        std::string("1 9"));
}

// stdlib §2.15: `input` / `options` bind by name.  Pre-fix: the options
// struct was lowered as the input and the program crashed.
BEZEL_TEST(builtin_ctor_named_arg_7498,
           jsondeserializer_reordered_named_args_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "import System.Serialization;", "",
            "    var opts := new JsonOptions(false, 2, false, false, 64); "
            "    using var d := new JsonDeserializer(options := opts, "
            "                                        input := \"7\"); "
            "    Console.WriteLine(d.ReadInt()); ")),
        std::string("7"));
}

// stdlib §2.15: `input` / `ignoreUnknownFields` bind by name.
BEZEL_TEST(builtin_ctor_named_arg_7498,
           binarydeserializer_reordered_named_args_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "import System.Serialization;", "",
            "    using var s := new BinarySerializer(); "
            "    s.WriteInt(42); "
            "    using var bytes := s.ToBytes(); "
            "    using var d := new BinaryDeserializer("
            "        ignoreUnknownFields := true, "
            "        input := bytes.AsReadOnlySpan()); "
            "    Console.WriteLine(d.ReadInt()); ")),
        std::string("42"));
}

// stdlib §2.15 declares `ignoreUnknownFields := false`: a NAMED one-argument
// construction is legal (the registry row used to demand both parameters).
BEZEL_TEST(builtin_ctor_named_arg_7498,
           binarydeserializer_named_input_alone_is_admitted) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "import System.Serialization;", "",
            "    using var s := new BinarySerializer(); "
            "    s.WriteInt(42); "
            "    using var bytes := s.ToBytes(); "
            "    using var d := new BinaryDeserializer("
            "        input := bytes.AsReadOnlySpan()); "
            "    Console.WriteLine(d.ReadInt()); ")),
        std::string("42"));
}

// stdlib §4.14: `ArraySpan<T>(Source, Start, Length)` by name.  Pre-fix: the
// swapped values failed IR verification.
BEZEL_TEST(builtin_ctor_named_arg_7498,
           arrayspan_reordered_named_args_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "", "",
            "    using var arr := new Array<int>(); "
            "    arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); "
            "    var sp := new ArraySpan<int>(Length := 2, Start := 1, "
            "                                 Source := arr); "
            "    Console.WriteLine($\"{sp.Length} {sp[0]} {sp[1]}\"); ")),
        std::string("2 20 30"));
}
