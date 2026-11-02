// Spec-derived regression test for issue #7957 (campaign builtin-call-admission,
// lane C2-56): a nullable `int?` argument at an `int` position of a built-in
// constructor was accepted silently and lowered unsoundly (the #7820 / #7928
// class: `checkBuiltinCtorArgTypes` skipped any nullable argument as "the
// nullability gate's", but the gate names no position of these rows).
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// spec §5.1 "Implicit conversions", the Nullable-conversions paragraph: "Each
//   implicit conversion of this section also lifts to the nullable forms: `S?`
//   implicitly converts to `T?` whenever `S` implicitly converts to `T`" --
//   the lifting is one-way, so the list has NO implicit `T?` to `T`
//   conversion.
// spec §7.1: "The compiler enforces the distinction at every assignment,
//   parameter pass, and return"; "A non-nullable reference type implicitly
//   converts to its nullable form. The reverse requires a null check or `!`."
//   §7.3: "Assigning `null` to a non-nullable target is a compile error."
//   §7.4 "Member access on nullable values", "How narrowing is established":
//   a fact is established "from a declaration whose initializer is such an
//   `expr` -- `var x: T? := expr;` or `const x: T? := expr;`".
// spec §22.6 "Overloading": "The argument's static type is its type at the
//   call: a read of a binding §7.4 has narrowed non-null is of its
//   non-nullable type" -- so a NARROWED read is of its non-nullable type and
//   the program is legal; only the carrier had to be extracted.
// stdlib §4.14 `Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)`,
//   `ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)` and
//   `OwningBuffer<T>(length: int)`; §4.15 `ArraySpan<T>(Source: Array<T>,
//   Start: int, Length: int)` and `ReadOnlyArraySpan<T>`; §3.3
//   `ReadOnlyStringSpan(Source: string, Start: int, Length: int)`; §4.5
//   `OwningArray<T>(capacity: int)`; §2.15 `JsonOptions(Indented: bool,
//   IndentWidth: int, AllowTrailingCommas: bool, IgnoreUnknownFields: bool,
//   MaxDepth: int)`; §2.7 `Random(seed: long)`; §6.3 `Semaphore(initialCount:
//   int)`; §6.9 `Channel<T>(capacity: int)`: every position is non-nullable.
//
// --- THE DEFECT (MEASURED on r30-c1 db9fed30) --------------------------------
//
// `new Span<int>(a, 1, c)` over `c: int? := 2` compiled and ran as an ASan
// stack-buffer-overflow (the {i8, i32} carrier read as an i32 length) and
// then SEGFAULTED (run exit 139); `new JsonOptions(true, n, false, false, 3)`
// over `n: int?` failed IR verification ("Invalid InsertValueInst
// operands!").

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Collections; "
           "import System.Text; import System.Serialization; "
           "class C "
           "  public static method Id(x: int): int? return x; end Id; "
           "  public static method IdL(x: long): long? return x; end IdL; "
           "  public static method IdB(x: bool): bool? return x; end IdB; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

void expectNullableRefused(const std::string& body, const char* param,
                           const char* head) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the `") + param + "` argument of the `" + head +
            "` constructor is nullable but the target type is non-nullable "
            "(\xC2\xA7""7)").c_str()));
}

// A program that reads a JsonOptions back through the serializer's output
// (the byte count of an indented record), so the IndentWidth that reached the
// value is observable without a member read.
std::string jsonProg(const std::string& body) {
    return "module M; import System; import System.Collections; "
           "import System.Text; import System.Serialization; "
           "record P(X: int, Y: int): ISerializable, IDeserializable; "
           "class C "
           "  public static method Id(x: int): int? return x; end Id; "
           "  public static method Len(o: JsonOptions): int "
           "    using var s := new JsonSerializer(o); "
           "    var p := new P(3, 7); p.Serialize(s); "
           "    using var bytes := s.ToBytes(); return bytes.Length; "
           "  end Len; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The same program with a CLASS element type, for `OwningArray<T>` (an owning
// collection admits no primitive element -- stdlib §4.5).
std::string boxProg(const std::string& body) {
    return "module M; import System; import System.Collections; "
           "import System.Text; import System.Serialization; "
           "class Box public x: int := 0; end Box; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(builtin_ctor_nullable_int_arg_7957, span_nullable_length_refused) {
    // The issue's repro (w_nullable.bzl): an ASan stack-buffer-overflow.
    expectNullableRefused(
        "    using var a := new OwningBuffer<int>(10); var c: int? := Id(2); "
        "    var sp := new Span<int>(a, 1, c); Console.WriteLine(sp.Length);",
        "Length", "Span");
}
BEZEL_TEST(builtin_ctor_nullable_int_arg_7957, window_family_nullable_start_refused) {
    expectNullableRefused(
        "    using var a := new OwningBuffer<int>(10); var c: int? := Id(2); "
        "    var sp := new ReadOnlySpan<int>(a, c, 1);",
        "Start", "ReadOnlySpan");
    expectNullableRefused(
        "    var a := new Array<int>(10, 1); var c: int? := Id(2); "
        "    var sp := new ArraySpan<int>(a, c, 1);",
        "Start", "ArraySpan");
    expectNullableRefused(
        "    var a := new Array<int>(10, 1); var c: int? := Id(2); "
        "    var sp := new ReadOnlyArraySpan<int>(a, 1, c);",
        "Length", "ReadOnlyArraySpan");
    expectNullableRefused(
        "    var c: int? := Id(2); "
        "    var sp := new ReadOnlyStringSpan(\"hello world\", c, 5);",
        "Start", "ReadOnlyStringSpan");
}
BEZEL_TEST(builtin_ctor_nullable_int_arg_7957, json_options_nullable_refused) {
    // The issue's repro (j_nullable.bzl): IR verification failure.
    expectNullableRefused(
        "    var n: int? := Id(3); "
        "    var o := new JsonOptions(true, n, false, false, 3); "
        "    Console.WriteLine(o.IndentWidth);",
        "IndentWidth", "JsonOptions");
    expectNullableRefused(
        "    var n: int? := Id(3); "
        "    var o := new JsonOptions(true, 2, false, false, n);",
        "MaxDepth", "JsonOptions");
    // The `bool` components are non-nullable too.
    expectNullableRefused(
        "    var b: bool? := IdB(true); "
        "    var o := new JsonOptions(b, 2, false, false, 3);",
        "Indented", "JsonOptions");
}
BEZEL_TEST(builtin_ctor_nullable_int_arg_7957, other_int_positions_refused) {
    // The same skip covered every kBPTInt / kBPTLong row with no mask bit.
    expectNullableRefused(
        "    var n: int? := Id(3); using var b := new OwningBuffer<int>(n);",
        "length", "OwningBuffer");
    expectNullableRefused(
        "    var n: int? := Id(3); using var ch := new Channel<int>(n);",
        "capacity", "Channel");
    expectNullableRefused(
        "    var n: int? := Id(3); var s := new Semaphore(n);",
        "initialCount", "Semaphore");
    expectNullableRefused(
        "    var n: long? := IdL(3); var r := new Random(n);", "seed", "Random");
}
// The issue's two repros as written. `var c: int? := 2;` is a declaration whose
// initializer is non-null, so spec section 7.4 (assignment narrowing, "from a
// declaration whose initializer is such an expr") narrows `c` non-null: the
// read is of type `int` (section 22.6) and the program is LEGAL. The defect
// there was the unextracted `{i8 hasValue; i32}` carrier (ASan stack-buffer-
// overflow at Span, IR verification failure at JsonOptions), not a missing
// rejection: the program must run and print the value.
BEZEL_TEST(builtin_ctor_nullable_int_arg_7957, initializer_narrowed_repros_run) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a := new OwningBuffer<int>(10); var c: int? := 2; "
        "    var sp := new Span<int>(a, 1, c); Console.WriteLine(sp.Length);")),
        "2");
    // The JsonOptions repro: `n` narrowed by its initializer, read at
    // `IndentWidth`. The carrier is extracted, so the serializer indents by 3
    // exactly as it does for a plain `int` 3 (the two byte counts agree).
    BEZEL_EXPECT_EQ(runAotOut(jsonProg(
        "    var n: int? := 3; var i: int := 3; "
        "    Console.WriteLine(Len(new JsonOptions(true, n, false, false, 3)) = "
        "                      Len(new JsonOptions(true, i, false, false, 3))); "
        "    Console.WriteLine(Len(new JsonOptions(true, 3, false, false, 3)) <> "
        "                      Len(new JsonOptions(true, 9, false, false, 3)));")),
        "True\nTrue");
    // The same Span repro under the CHECKED oracle, where the generated code is
    // ASan-instrumented: the base's `{i8 hasValue; i32}` carrier read as an i32
    // length was an ASan stack-buffer-overflow (and then a SIGSEGV) there, so
    // this leg is where the payload extraction is proved. `runAotChecked`
    // returns ran=false on any sanitizer fire (ASan or LSan).
    auto r = runAotChecked(prog(
        "    using var a := new OwningBuffer<int>(10); var c: int? := 2; "
        "    var sp := new Span<int>(a, 1, c); Console.WriteLine(sp.Length);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_EQ("2\n", r.stdoutText);
}
// Controls: int, a narrowed read, `c!` and constants construct and run.
BEZEL_TEST(builtin_ctor_nullable_int_arg_7957, accept_controls) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    using var a := new OwningBuffer<int>(10); "
        "    var i: int := 3; var c: int? := 4; "
        "    var s1 := new Span<int>(a, 1, i); Console.WriteLine(s1.Length); "
        "    var s2 := new Span<int>(a, 1, c!); Console.WriteLine(s2.Length); "
        "    if c is not null then "
        "      var s3 := new Span<int>(a, 1, c); Console.WriteLine(s3.Length); "
        "    end if; "
        "    var s4 := new Span<int>(a, 1, 5); Console.WriteLine(s4.Length); "
        "    var d: int? := Id(7); "
        "    if d is not null then "
        "      var s5 := new Span<int>(a, 1, d); Console.WriteLine(s5.Length); "
        "    end if;")),
        "3\n4\n4\n5\n7");
    BEZEL_EXPECT_EQ(runAotOut(jsonProg(
        "    var i: int := 6; var n: int? := 6; var m: int? := Id(6); "
        "    var baseLen := Len(new JsonOptions(true, i, false, false, 3)); "
        "    Console.WriteLine(Len(new JsonOptions(true, n!, false, false, 3)) = baseLen); "
        "    if m is not null then "
        "      Console.WriteLine(Len(new JsonOptions(true, m, false, false, m)) = baseLen); "
        "    end if; "
        "    Console.WriteLine(Len(new JsonOptions(true, 6, false, false, 3)) = baseLen);")),
        "True\nTrue\nTrue");
}
// A `null` LITERAL is a nullable expression too (spec §7.3: "Assigning `null`
// to a non-nullable target is a compile error"), so it is refused at every
// non-nullable position this fix covers -- the `JsonOptions` components
// included, where the base admitted it (the position has no row in the
// parameter table, so the §7.3 gate names it neither).
BEZEL_TEST(builtin_ctor_nullable_int_arg_7957, null_literal_refused) {
    expectNullableRefused(
        "    var o := new JsonOptions(null, 2, false, false, 3);",
        "Indented", "JsonOptions");
    expectNullableRefused(
        "    var o := new JsonOptions(true, null, false, false, 3);",
        "IndentWidth", "JsonOptions");
    expectNullableRefused(
        "    using var a := new OwningBuffer<int>(10); "
        "    var sp := new Span<int>(a, 1, null);",
        "Length", "Span");
}
// The narrowed read is published at EVERY position the fix covers, not only at
// the two the issue names: the `{i8 hasValue; T}` carrier is extracted, so the
// value that reaches the constructor is the payload. Each row below READS the
// value back (the buffer's extent, the array's capacity, the serializer's byte
// count, the seed's sequence), so a fix that published the wrong payload would
// be seen here.
BEZEL_TEST(builtin_ctor_nullable_int_arg_7957, narrowed_reads_run_at_every_position) {
    // OwningBuffer: the narrowed length is the buffer's extent.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var n: int? := 5; using var b := new OwningBuffer<int>(n); "
        "    Console.WriteLine(b.Length);")),
        "5");
    // OwningArray: the narrowed capacity is the array's capacity.
    BEZEL_EXPECT_EQ(runAotOut(boxProg(
        "    var n: int? := 6; using var a := new OwningArray<Box>(n); "
        "    Console.WriteLine(a.Capacity);")),
        "6");
    // Random: a seeded generation is deterministic (stdlib §2.7), so the
    // narrowed seed must give the SAME sequence as the plain `long` seed.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var n: long? := 5; using var r1 := new Random(n); "
        "    using var r2 := new Random(5L); "
        "    Console.WriteLine(r1.NextInt64() = r2.NextInt64());")),
        "True");
    // JsonOptions' bool component: the narrowed value reaches the record, so
    // the serializer's output matches the plain `bool`'s.
    BEZEL_EXPECT_EQ(runAotOut(jsonProg(
        "    var b: bool? := true; var i: bool := true; "
        "    Console.WriteLine(Len(new JsonOptions(b, 2, false, false, 3)) = "
        "                      Len(new JsonOptions(i, 2, false, false, 3)));")),
        "True");
    // Semaphore and Channel: the narrowed count / capacity reaches the
    // constructor (the base failed IR verification on the carrier itself).
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var n: int? := 1; using var s := new Semaphore(n); "
        "    using var ch := new Channel<int>(n); "
        "    Console.WriteLine(1);")),
        "1");
}
