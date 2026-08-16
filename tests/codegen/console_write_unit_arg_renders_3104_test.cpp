// fix #3104 — `Console.WriteLine(<unit-typed expr>)` was silently DROPPED by
// the value-demanding guard added at #3079, while the sibling branch (b) of
// the same guard HARD-ERRORED on a comparable value-less shape. The two
// branches of the conditional disagreed: a typed-unit argument vanished,
// an untyped-receiver-argument construct refused to compile.
//
// §4.7, verbatim: "The unit type is a value type and can in principle be
// boxed via the `Object` derivation chain (§8.6) — `var o: Object := ();` is
// grammatically valid and produces a boxed `()`." §8.5 explicitly renders
// `()` as the literal "()". stdlib §2.1, verbatim:
//   `public static method WriteLine(value: Object);`
// so `Console.WriteLine(<unit>)` has a spec-defined rendering the compiler
// was throwing away. §21.9 grants the unit-result EXPRESSION-STATEMENT
// permission and does not prohibit argument position; §8.5 / §4.7 positively
// admit the rendering the call site asks for.
//
// THE DEFECT was codegen-side, in `tryLowerConsoleCall`
// (src/codegen/LlvmBackend_Calls.cpp). The guard #3079 added for
// `lowerExpr`-returning-nullptr split into three branches — (b) hard Error,
// (c) Warning, (a) silent unit exemption — and branch (a) was the defective
// one: it ignored the spec's "()"-rendering directive because `()` lowers to
// LLVM `void` (§2915), so "no lowered value" IS the type's representation,
// not a lowering failure. The pre-fix branch restored here makes
// `tryLowerConsoleCall` continue past the guard with `arg` set to a
// sentinel-rc `"()"` string literal, mirroring the nullable-value "null"
// arm above it (same `lowerStringLiteral` path, phied into a `ptrTy()`
// slot before `emitConsoleWrite`).
//
// The tests assert the FULL stdout rather than an exit code, because the
// reported symptom IS missing output: a `<`/`>` frame around each shape
// makes a dropped line fail as a wrong string instead of passing as a
// wrong-but-plausible value.
//
// Spec: §4.7 / §8.5 / §21.9, stdlib §2.1, v0.255.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// Shape (i): a method returning `()` whose body calls another, called
// from `Console.WriteLine`. The frame `<`/`>` pins both the silent drop
// (the pre-fix symptom) AND the rendered "()" (the post-fix behavior) —
// the test fails as a wrong string either way.
BEZEL_TEST(codegen_console_write_unit_arg_renders_3104,
           unit_method_arg_prints_spec_4_7_8_5_21_9) {
    auto r = runAot(
        "module N3104A; "
        "class C "
        "  public static method ReturnsUnit(): () "
        "    return; "
        "  end ReturnsUnit; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"<\"); "
        "    Console.WriteLine(ReturnsUnit()); "
        "    Console.WriteLine(\">\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("<\n()\n>\n"));
}

// Shape (ii): the literal unit expression `()`, with no enclosing call. §4.7
// admits the boxing directly; the rendered text is the same single token.
BEZEL_TEST(codegen_console_write_unit_arg_renders_3104,
           unit_literal_arg_prints_spec_4_7_8_5) {
    auto r = runAot(
        "module N3104B; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(\"<\"); "
        "    Console.WriteLine(()); "
        "    Console.WriteLine(\">\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("<\n()\n>\n"));
}

// Shape (iii): `Console.Write` (no Line) — the same lowering path, the
// same guard, the same §8.5 answer. Pinned explicitly so the fix's path
// cannot silently regress to "renders only on WriteLine".
BEZEL_TEST(codegen_console_write_unit_arg_renders_3104,
           unit_arg_via_console_write_only_spec_2_1) {
    auto r = runAot(
        "module N3104C; "
        "class C "
        "  public static method ReturnsUnit(): () "
        "    return; "
        "  end ReturnsUnit; "
        "  public static method Main(): int "
        "    Console.Write(\"<\"); "
        "    Console.Write(ReturnsUnit()); "
        "    Console.WriteLine(\">\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("<()>\n"));
}

// Pre-empt — REGRESSION GUARD. The unit-typed ARGUMENT must not poison the
// sibling value-demanding shapes the guard was added for. Verified by
// pinning the §3079 sibling (c) — the §3 permissive `o.Anything()` — still
// emits its Warning, and the unit-typed frame STILL renders "()". The
// §3079 sibling (b) — built-in receiver that yields nothing — lives on
// the lit side (`tests/lit/`) because runAot's whole-binary compile
// approach dies on a hard Error before any test observable survives, and
// a `ChannelSender<int>` instance would also pull in a runtime channel
// out of scope for a one-fix lane. The negative-control chosen here is
// the §3 permissive shape: same guard area, non-error severity, AND
// string interpolation that the runAot harness can capture.
//
// What this gtest pins:
//   • Console.WriteLine(<unit>) prints "()"        (the new render)
//   • Console.WriteLine(o) with `o: Object := "ok"` prints "ok"
//   • The §3079 permissive-anything line still fires its Warning
//     BEFORE the write (so the EXPECT_EQ captures only stdout, no
//     stderr, and the existing harness ignores the diagnostic).
BEZEL_TEST(codegen_console_write_unit_arg_renders_3104,
           sibling_3079_permissive_still_warns_spec_3_8_5_21_9) {
    auto r = runAot(
        "module N3104D; "
        "class C "
        "  public static method ReturnsUnit(): () "
        "    return; "
        "  end ReturnsUnit; "
        "  public static method Main(): int "
        "    var o: Object := \"ok\"; "
        "    Console.WriteLine(\"<\"); "
        "    Console.WriteLine(ReturnsUnit()); "
        "    Console.WriteLine(o); "
        "    Console.WriteLine(\">\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("<\n()\nok\n>\n"));
}
