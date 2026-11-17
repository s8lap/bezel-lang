// Codegen tests — §9.8 v0.357 synthesized `Deserialize`: one emitted copy of
// the reader that RUNS MORE THAN ONCE in a frame must start every run from a
// clean slate (issue #8574).
//
// A synthesized `Deserialize` body is emitted INLINE at its call site, so one
// emitted copy runs many times in one frame whenever the call sits in a loop
// (and — issue #8391's F6 — whenever it is the element reader inside a
// collection component's element loop). Before the fix the reader's scratch
// slots were reset once per FUNCTION, in the entry block, not once per RUN,
// and three things went wrong:
//
//   1. THE ISSUE'S OWN DEFECT. A run that completes moves its component
//      values into the constructed object, but the pointers stay behind in
//      the component slots. A LATER run that raises `SerializationException`
//      before it overwrites such a slot has its unwind sweep release the
//      stale pointer — a pointer the earlier object already owns, or has
//      already freed: `refcount over-release (string)` for a string
//      component, AddressSanitizer heap-use-after-free / double free for an
//      owned class or collection component.
//
//   2. THE CLEANUP SLOTS. The field-name slot (and the `$case` / `$type`
//      discriminator slot of a choice / polymorphic read) is released by the
//      unwind sweep without being cleared, so a later run that raises before
//      it reads a name releases the same name again. This one needs no
//      owning component at all: a class of two `int`s aborts.
//
//   3. A WRONG VALUE ON A SUCCESSFUL READ. §9.8 v0.357, `serialized
//      optional`: "On read, a missing field yields `default(T)` instead of
//      raising `SerializationException`". A run whose wire omits an optional
//      component took the PREVIOUS run's slot value instead: an `int?` read
//      back as the earlier object's number, and a `string?` aliased the
//      earlier object's string without a reference of its own.
//
// What the spec requires: §9.8 v0.357 — "A **missing** required component
// (one neither present nor `serialized optional`), a **duplicate** field
// name, a type mismatch, or a name not matching any enum constant raises
// `SerializationException` unconditionally"; §28.3 v0.357 — "**Owned** — the
// holder is responsible for disposing the referenced object exactly once"
// (the reader holds each component it has read until it hands it to the
// constructed object, so on the exception path it disposes exactly those —
// not an earlier run's); §28.9 v0.357 — a `string` is runtime-managed ("the
// type's lifetime is managed by the runtime without user involvement"), so a
// program can never be the cause of the runtime's over-release abort.
//
// Every row runs under `runAotChecked` (AddressSanitizer over the generated
// module, LeakSanitizer at exit) and asserts BOTH `didNotCrash` (ran to
// completion with the exact output; no AddressSanitizer report; no §28.9
// over-release abort) and `assertNoLeak` (LeakSanitizer counted 0 leaked
// allocations). The verbatim two-call repro aside, each failing read is
// driven at least 200 times, so a one-allocation leak cannot hide under a
// one-shot repro.
//
// Two PRE-EXISTING leaks on the exception path are NOT this issue and are
// kept out of the rows that would otherwise trip them: #8585 (a truncated
// binary choice / polymorphic payload leaks the exception object) and #8586
// (a truncated binary `Shared<T>` component leaks the box). The rows say
// where they steer around them.
//
// BinarySerializer/BinaryDeserializer and JsonSerializer/JsonDeserializer
// live in System.Serialization (stdlib §2.15) and are NOT auto-imported.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort (SIGABRT from the over-release
// check) and any sanitizer report as `ran == false`.
void didNotCrash(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, const std::string& out) {
    didNotCrash(r, out);
    assertNoLeak(r);
}

// ---------------------------------------------------------------------------
// The program builders. A shape is the type declarations, the statements that
// bind `src` (the value to serialize), and how the read-back local is bound
// (`using var` for a class, `var` for a record or a choice). The root type is
// always `P`.
// ---------------------------------------------------------------------------
struct Shape {
    const char* decls;
    const char* build;
    const char* bind;
};

const char* kHead = "module M; import System.Serialization;\n";

// THE CALL LOOP, every ordered pair. ONE textual `P.Deserialize(d)` inside
// loops in `Main`, driven through every ORDERED PAIR (a, b) of truncation
// points 0..n (n = the whole payload): the same emitted copy sees
// success -> failure, failure -> success and failure -> failure with the
// failure at every offset on both sides. §9.8 v0.357: a strict prefix of a
// payload is missing a required component (or is malformed before one), so
// it raises; the whole payload round-trips. The program checks both counts
// itself and prints two booleans.
//
// `skipLo..skipHi` (inclusive) are cuts replaced by cut 0 — used only to
// steer around the pre-existing leaks #8585 / #8586 named in the header.
// Each cut is driven 2 * (n + 1) * rounds times on each side of the pair;
// every row picks `rounds` so that is at least 200.
std::string callLoopEveryPair(const Shape& sh, const char* format, int rounds,
                              int skipLo = -1, int skipHi = -1) {
    std::string skip;
    if (skipLo >= 0)
        skip = "            if cut >= " + std::to_string(skipLo) +
               " then if cut <= " + std::to_string(skipHi) +
               " then cut := 0; end; end;\n";
    const std::string r = std::to_string(rounds);
    return std::string(kHead) + sh.decls + R"BZL(
class C
  public static method Main(): int
    )BZL" + sh.build + R"BZL(
    using var s := new )BZL" + format + R"BZL(Serializer();
    src.Serialize(s);
    using var bytes := s.ToBytes();
    var n := bytes.Length;
    var caught := 0;
    var ok := 0;
    var round := 0;
    while round < )BZL" + r + R"BZL( do
      var a := 0;
      while a <= n do
        var b := 0;
        while b <= n do
          var k := 0;
          while k < 2 do
            var cut := a;
            if k = 1 then cut := b; end;
)BZL" + skip + R"BZL(            using var d := new )BZL" + format + R"BZL(Deserializer(bytes.AsReadOnlySpan(0, cut));
            try
              )BZL" + sh.bind + R"BZL( x := P.Deserialize(d);
              ok := ok + 1;
            catch SerializationException => caught := caught + 1;
            end;
            k := k + 1;
          end;
          b := b + 1;
        end;
        a := a + 1;
      end;
      round := round + 1;
    end;
    var wantOk := )BZL" + r + R"BZL( * 2 * (n + 1);
    var wantCaught := )BZL" + r + R"BZL( * 2 * (n + 1) * (n + 1) - wantOk;
    Console.WriteLine(ok = wantOk);
    Console.WriteLine(caught = wantCaught);
    return 42;
  end Main;
end C;
)BZL";
}

// THE CALL LOOP, every prefix in order: 200 rounds of cut = 0, 1, .. n. The
// whole payload (cut = n) succeeds once per round and is followed by cut = 0
// of the next round — a completed run, then a run that raises before it
// reads anything.
std::string callLoopEveryPrefix(const Shape& sh, const char* format) {
    return std::string(kHead) + sh.decls + R"BZL(
class C
  public static method Main(): int
    )BZL" + sh.build + R"BZL(
    using var s := new )BZL" + format + R"BZL(Serializer();
    src.Serialize(s);
    using var bytes := s.ToBytes();
    var n := bytes.Length;
    var caught := 0;
    var ok := 0;
    var round := 0;
    while round < 200 do
      var cut := 0;
      while cut <= n do
        using var d := new )BZL" + format + R"BZL(Deserializer(bytes.AsReadOnlySpan(0, cut));
        try
          )BZL" + sh.bind + R"BZL( x := P.Deserialize(d);
          ok := ok + 1;
        catch SerializationException => caught := caught + 1;
        end;
        cut := cut + 1;
      end;
      round := round + 1;
    end;
    Console.WriteLine(ok);
    Console.WriteLine(caught = 200 * n);
    return 42;
  end Main;
end C;
)BZL";
}

// THE ISSUE'S LOOP, `calls` times: even calls read the whole payload, odd
// calls read 1 byte (which raises before any component is reached). With
// `calls` = 2 this is the issue's repro verbatim.
std::string wholeThenOneByte(const char* decls, const char* build, int calls) {
    const std::string c = std::to_string(calls);
    return std::string(kHead) + decls + R"BZL(
class C
  public static method Main(): int
    )BZL" + build + R"BZL(
    using var s := new BinarySerializer();
    good.Serialize(s);
    using var bytes := s.ToBytes();
    var caught := 0;
    var ok := 0;
    var i := 0;
    while i < )BZL" + c + R"BZL( do
      var n := bytes.Length;
      if i mod 2 = 1 then n := 1; end;
      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, n));
      try
        using var b2 := Bag.Deserialize(d);
        ok := ok + 1;
      catch SerializationException => caught := caught + 1;
      end;
      i := i + 1;
    end;
    Console.WriteLine(ok);
    Console.WriteLine(caught);
    return 42;
  end Main;
end C;
)BZL";
}

// The issue's own types: an `owned` class component.
const char* kIssueDecls = R"BZL(class Leaf(v: int): ISerializable, IDeserializable
  public property V: int => v;
end Leaf;
class Bag(id: int, owned l: Leaf): ISerializable, IDeserializable
  public owned L: Leaf := l;
  public property Id: int => id;
end Bag;)BZL";
const char* kIssueBuild = "using var good := new Bag(7, new Leaf(3));";

// The issue's second form: a `string` component in place of the class.
const char* kStringDecls = R"BZL(class Bag(id: int, s: string): ISerializable, IDeserializable
  public S: string := s;
  public property Id: int => id;
end Bag;)BZL";
const char* kStringBuild = "using var good := new Bag(7, \"he\" + \"llo\");";

const Shape kThreeStrings = {
    R"BZL(class P(s1: string, s2: string, s3: string): ISerializable, IDeserializable
  public S1: string := s1;
  public S2: string := s2;
  public S3: string := s3;
end P;)BZL",
    "using var src := new P(\"al\" + \"pha\", \"be\" + \"ta\", \"ga\" + \"mma\");",
    "using var"};

// No owning component at all: the only pointer the reader holds is the field
// name it is matching.
const Shape kTwoInts = {
    R"BZL(class P(a: int, b: int): ISerializable, IDeserializable
  public property A: int => a;
  public property B: int => b;
end P;)BZL",
    "using var src := new P(7, 9);", "using var"};

const Shape kStringIntString = {
    R"BZL(class P(s1: string, n: int, s2: string): ISerializable, IDeserializable
  public S1: string := s1;
  public property N: int => n;
  public S2: string := s2;
end P;)BZL",
    "using var src := new P(\"al\" + \"pha\", 3, \"be\" + \"ta\");", "using var"};

const Shape kOwnedClassAndString = {
    R"BZL(class Leaf(name: string, v: int): ISerializable, IDeserializable
  public Name: string := name;
  public property V: int => v;
end Leaf;
class P(id: int, owned l: Leaf, s: string): ISerializable, IDeserializable
  public owned L: Leaf := l;
  public property Id: int => id;
  public S: string := s;
end P;)BZL",
    "using var src := new P(7, new Leaf(\"le\" + \"af\", 3), \"ta\" + \"il\");",
    "using var"};

const Shape kNullableOwnedClass = {
    R"BZL(class Leaf(name: string, v: int): ISerializable, IDeserializable
  public Name: string := name;
  public property V: int => v;
end Leaf;
class P(owned l: Leaf?, s: string, owned m: Leaf?): ISerializable, IDeserializable
  public owned L: Leaf? := l;
  public S: string := s;
  public owned M: Leaf? := m;
end P;)BZL",
    "using var src := new P(new Leaf(\"le\" + \"af\", 3), \"ta\" + \"il\", null);",
    "using var"};

const Shape kIntRecord = {
    "record P(A: int, B: int): ISerializable, IDeserializable;",
    "var src := new P(7, 9);", "var"};

const Shape kChoice = {
    R"BZL(choice P: ISerializable, IDeserializable
  Note(text: string, n: int, author: string), Ping;
end P;)BZL",
    "var src := P.Note(\"te\" + \"xt\", 5, \"au\" + \"th\");", "var"};

const Shape kPolymorphic = {
    R"BZL(abstract class Shape : ISerializable, IDeserializable
  polymorphic("circle": Circle, "square": Square);
  public abstract method Kind(): int;
end Shape;
class Circle : Shape, ISerializable, IDeserializable
  public property Name: string;
  public property Radius: double;
  public constructor(name: string, radius: double) Name := name; Radius := radius; end constructor;
  public override method Kind(): int return 1; end Kind;
end Circle;
class Square : Shape, ISerializable, IDeserializable
  public property Side: double;
  public constructor(side: double) Side := side; end constructor;
  public override method Kind(): int return 2; end Kind;
end Square;
class P : ISerializable, IDeserializable
  public owned S: Shape;
  public property T: string;
  public constructor(owned s: Shape, t: string) S := s; T := t; end constructor;
end P;)BZL",
    "using var src := new P(new Circle(\"ci\" + \"rc\", 5.0), \"ta\" + \"il\");",
    "using var"};

const Shape kSharedGraph = {
    R"BZL(class Inner : ISerializable, IDeserializable
  public property V: int;
  public property W: string;
  public constructor(v: int, w: string) V := v; W := w; end constructor;
end Inner;
class P : ISerializable, IDeserializable
  public owned Handle: Shared<Inner>;
  public owned Again: Shared<Inner>;
  public property T: string;
  public constructor(owned handle: Shared<Inner>, owned again: Shared<Inner>, t: string)
    Handle := handle; Again := again; T := t;
  end constructor;
end P;)BZL",
    R"BZL(using var h0 := new Shared<Inner>(new Inner(7, "w" + "w"));
    using var src := new P(h0.Clone(), h0.Clone(), "ta" + "il");)BZL",
    "using var"};

// `serialized optional` on a nullable auto-property: §9.8 v0.357 omits the
// field on write when the value is null, and "On read, a missing field
// yields `default(T)`". `%T%` is the component type, `%V%` a non-null value.
// Even calls carry the value, odd calls omit it; the program counts how many
// read-backs are null and how many are not.
std::string optionalOnAlternateCalls(const char* type, const char* value) {
    std::string p = std::string(kHead) + R"BZL(
class Meta : ISerializable, IDeserializable
  public property Code: int;
  public property Note: %T%? serialized optional;
  public constructor(code: int, note: %T%?)
    Code := code; Note := note;
  end constructor;
end Meta;
class C
  public static method Main(): int
    var i := 0;
    var nulls := 0;
    var values := 0;
    while i < 400 do
      var note: %T%? := null;
      if i mod 2 = 0 then note := %V%; end;
      using var m := new Meta(i, note);
      using var s := new BinarySerializer();
      m.Serialize(s);
      using var bytes := s.ToBytes();
      using var d := new BinaryDeserializer(bytes.AsReadOnlySpan());
      using var m2 := Meta.Deserialize(d);
      if m2.Note = null then nulls := nulls + 1; else values := values + 1; end;
      i := i + 1;
    end;
    Console.WriteLine(values);
    Console.WriteLine(nulls);
    return 42;
  end Main;
end C;
)BZL";
    auto replaceAll = [&p](const std::string& from, const std::string& to) {
        for (std::size_t at = p.find(from); at != std::string::npos;
             at = p.find(from, at + to.size()))
            p.replace(at, from.size(), to);
    };
    replaceAll("%T%", type);
    replaceAll("%V%", value);
    return p;
}

}  // namespace

// ===========================================================================
// 1. The issue's own defect: a completed run's component pointer, released
//    again by a later run's unwind sweep.
// ===========================================================================

// THE ISSUE'S OWN REPRO, verbatim (two calls: the whole payload, then one
// byte). Pre-fix: AddressSanitizer heap-use-after-free. §9.8 v0.357: the
// second call raises `SerializationException` — ok = 1, caught = 1.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           issue_repro_owned_class_component_whole_then_one_byte_spec_9_8_v0357) {
    expectClean(runAotChecked(wholeThenOneByte(kIssueDecls, kIssueBuild, 2)),
                "1\n1\n");
}

// The same loop, 200 calls: 100 succeed and 100 raise, and every raising
// call follows a completed one.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           owned_class_component_200_alternating_calls_spec_9_8_v0357) {
    expectClean(runAotChecked(wholeThenOneByte(kIssueDecls, kIssueBuild, 200)),
                "100\n100\n");
}

// The issue's second form — a `string` component. Pre-fix: `bezel: fatal:
// refcount over-release (string)` (§28.9).
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           string_component_200_alternating_calls_spec_28_9_v0357) {
    expectClean(runAotChecked(wholeThenOneByte(kStringDecls, kStringBuild, 200)),
                "100\n100\n");
}

// The issue's sweep: every prefix of a three-string class, in order, in one
// loop — 200 rounds here (the issue measured 3). Each round's whole-payload
// read is followed by the next round's empty read.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           three_string_class_every_prefix_200_rounds_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPrefix(kThreeStrings, "Binary")),
                "200\nTrue\n");
}

// Every ordered pair of cuts, JSON: strings on both sides of an `int`.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           string_class_every_ordered_pair_of_cuts_json_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kStringIntString, "Json", 4)),
                "True\nTrue\n");
}

// Every ordered pair of cuts over an `owned` class component that itself
// holds a string, followed by a string — binary and JSON.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           owned_class_and_string_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(
        runAotChecked(callLoopEveryPair(kOwnedClassAndString, "Binary", 3)),
        "True\nTrue\n");
}

BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           owned_class_and_string_every_ordered_pair_of_cuts_json_spec_9_8_v0357) {
    expectClean(
        runAotChecked(callLoopEveryPair(kOwnedClassAndString, "Json", 3)),
        "True\nTrue\n");
}

// A NULLABLE owned class component, one present and one null: the null one
// must not pick up the present one's pointer from an earlier run.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           nullable_owned_class_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(
        runAotChecked(callLoopEveryPair(kNullableOwnedClass, "Binary", 3)),
        "True\nTrue\n");
}

// ===========================================================================
// 2. The cleanup slots: the field name and the `$case` / `$type`
//    discriminator, released by the unwind sweep and then released again.
// ===========================================================================

// A class of two `int`s — no owning component. Pre-fix: `refcount
// over-release (string)`, the string being the field name.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           int_only_class_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kTwoInts, "Binary", 4)),
                "True\nTrue\n");
}

// The record reader has the same field-name slot.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           int_only_record_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kIntRecord, "Binary", 4)),
                "True\nTrue\n");
}

// A choice: the `$case` discriminator slot and the payload field-name slot.
// Cuts 4..20 are replaced by cut 0: they trip the pre-existing exception
// leak #8585, which is not this defect.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           choice_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kChoice, "Binary", 2, 4, 20)),
                "True\nTrue\n");
}

// A polymorphic owned component: the `$type` discriminator slot. JSON is
// swept whole; in binary, cuts 13..31 are replaced by cut 0 (#8585 again).
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           polymorphic_component_every_ordered_pair_of_cuts_json_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kPolymorphic, "Json", 2)),
                "True\nTrue\n");
}

BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           polymorphic_component_every_ordered_pair_of_cuts_binary_spec_9_8_v0357) {
    expectClean(
        runAotChecked(callLoopEveryPair(kPolymorphic, "Binary", 2, 13, 31)),
        "True\nTrue\n");
}

// Two `Shared<T>` handles to one object, then a string — JSON, swept whole
// (the binary form trips the pre-existing leak #8586 and is left to it).
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           shared_graph_every_ordered_pair_of_cuts_json_spec_9_8_v0357) {
    expectClean(runAotChecked(callLoopEveryPair(kSharedGraph, "Json", 2)),
                "True\nTrue\n");
}

// ===========================================================================
// 3. `serialized optional`: an omitted component reads back as `default(T)`,
//    not as the previous run's value.
// ===========================================================================

// `int?` — a WRONG-OUTPUT row: no sanitizer fires pre-fix, the program just
// prints `400` / `0` (every omitted `Note` read back as the earlier 77).
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           optional_int_omitted_on_alternate_calls_reads_null_spec_9_8_v0357) {
    expectClean(runAotChecked(optionalOnAlternateCalls("int", "77")),
                "200\n200\n");
}

// `string?` — pre-fix the omitted `Note` aliased the previous object's
// string, already released: a SEGV under AddressSanitizer.
BEZEL_TEST(codegen_serialization_deserialize_stale_slot_8574,
           optional_string_omitted_on_alternate_calls_reads_null_spec_9_8_v0357) {
    expectClean(
        runAotChecked(optionalOnAlternateCalls("string", "\"he\" + \"llo\"")),
        "200\n200\n");
}
