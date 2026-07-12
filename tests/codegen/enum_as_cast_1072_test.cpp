// Spec-derived regression tests for #1072 / §17.4 / §8.6: `o as EnumT` unboxes a
// boxed enum from an Object to the nullable enum `EnumT?` — a tagged
// {hasValue, underlying-int} struct (§10.3 #740). Casting the unboxed value to
// its underlying int (`int((o as Color)!)`) previously crashed bezelc with an
// LLVM GEP assertion, because the `as` value-type unbox branch returned a BARE
// enum value instead of the tagged struct that the primitive/record branches
// build (and that `int(enum?)` / `<> null` / `!` consume). The enum branch now
// builds the same tagged struct.
//
// runJit returns the raw exit code (0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kColor =
    "module M; enum Color Red, Green, Blue; end Color; ";  // ordinals 0,1,2
}  // namespace

// The #1072 repro: box an enum into Object, `as`-unbox it, assert non-null, and
// cast to the underlying int. Color.Green = ordinal 1.
BEZEL_TEST(enum_as_cast_1072, as_unbox_then_int_green) {
    BEZEL_EXPECT_EQ(run(std::string(kColor) +
        "class C public static method Main(): int "
        "  var o: Object := Color.Green; "
        "  var d := o as Color; "
        "  var e: int := 0; "
        "  if d <> null then e := int(d!); end; "
        "  return e; "
        "end Main; end C;"), 1);
}

// A different ordinal end-to-end. Color.Blue = ordinal 2.
BEZEL_TEST(enum_as_cast_1072, as_unbox_then_int_blue) {
    BEZEL_EXPECT_EQ(run(std::string(kColor) +
        "class C public static method Main(): int "
        "  var o: Object := Color.Blue; "
        "  return int((o as Color)!); "
        "end Main; end C;"), 2);
}

// A FAILED cast (the Object holds a boxed int, not a Color) yields a null
// `Color?` — the unbox is guarded, not a crash. Returns 0 (d is null).
BEZEL_TEST(enum_as_cast_1072, as_failed_cast_is_null) {
    BEZEL_EXPECT_EQ(run(std::string(kColor) +
        "class C public static method Main(): int "
        "  var o: Object := 42; "
        "  var d := o as Color; "
        "  return 7 if d <> null else 0; "
        "end Main; end C;"), 0);
}

// The unboxed value compares equal to the original enum constant.
BEZEL_TEST(enum_as_cast_1072, as_unbox_compares_equal) {
    BEZEL_EXPECT_EQ(run(std::string(kColor) +
        "class C public static method Main(): int "
        "  var o: Object := Color.Green; "
        "  var d := o as Color; "
        "  return 9 if d = Color.Green else 0; "
        "end Main; end C;"), 9);
}
