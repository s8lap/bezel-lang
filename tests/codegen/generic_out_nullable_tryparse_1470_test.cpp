// Spec-derived tests for issue #1470 — `T.TryParse(s, out v)` dispatched through
// a constrained type parameter, forwarding its result into a generic `out T?`
// parameter, silently dropped the parsed value (§11.8, stdlib §2.19, §7.10).
//
// §11.8: a static interface member reached through a type parameter (`T.TryParse`
// with `where T: IParsable<T>`) dispatches to T's compiler-provided conformance
// at instantiation. stdlib §2.19: `IParsable<T>` declares
// `static method TryParse(s: string, out value: T?): bool` — the interface out
// parameter is the NULLABLE companion `T?`, distinct from the per-type CONCRETE
// member whose out is a bare `T` (stdlib §9.1: `out result: int`).
//
// THE BUG: the primitive TryParse codegen bridge (LlvmBackend_Calls2.cpp) stored
// the parsed value as a BARE scalar at offset 0 of the out slot. That is correct
// for a direct `int.TryParse(s, out var b)` (bare-int slot), but through the
// generic the out slot is the tagged `{i8 hasValue; T value}` companion — the
// bare store landed on the hasValue byte + padding and left the value field
// (offset 4) uninitialized. `TryIt<int>("12345", out var b)` reported ok=True yet
// `b!` read garbage. This is the CALLEE/nested-marshalling sibling of #1469 (the
// caller-side out-var typing/slot, already fixed): here the nested `T.TryParse`
// WRITE into the forwarded `out T?` mis-marshalled.
//
// THE FIX marshals the parsed value into the companion (value into field 1,
// hasValue = the TryParse success bool — the v0.171 nullable-out convention: the
// false path reads as null) whenever the out slot's element type is that tagged
// struct; a bare out slot (the direct concrete-member path) is stored unchanged.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// The §11.8 forwarding wrapper: `T.TryParse` result flows into `out v: T?`.
const char* kWrapper =
    "module M; "
    "static class U "
    "  public static method TryIt<T>(s: string, out v: T?): bool "
    "      where T: IParsable<T> "
    "    return T.TryParse(s, out v); "
    "  end TryIt; "
    "end U; ";
}  // namespace

// The reported repro: T=int. Parse succeeds → ok=True, `out var b` is a non-null
// `int?` carrying 12345. Before the fix `b!` read uninitialized memory.
BEZEL_TEST(generic_out_nullable_tryparse_1470, int_value_delivered) {
    // 12345 does not fit an 8-bit exit code — observe on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var ok := U.TryIt<int>(\"12345\", out var b); "
        "    if not ok then return -2; end; "
        "    if b = null then return -1; end; "
        "    Console.WriteLine(b!); return 0; "
        "  end Main; "
        "end C;"), "12345");
}

// A WIDER value type (`long?`, {i8,i64}): the companion inner field is i64. The
// int-hardcoded store would have written i32; the fix marshals the full width.
BEZEL_TEST(generic_out_nullable_tryparse_1470, long_value_delivered) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var ok := U.TryIt<long>(\"12345\", out var b); "
        "    if not ok then return -2; end; "
        "    if b = null then return -1; end; "
        "    if b! = 12345L then return 55; end; "
        "    return -9; "
        "  end Main; "
        "end C;"), 55);
}

// A NARROWER value type (`byte?`, {i8,i8}): the tower's range-checked path also
// routes through the companion marshalling.
BEZEL_TEST(generic_out_nullable_tryparse_1470, byte_value_delivered) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var ok := U.TryIt<byte>(\"200\", out var b); "
        "    if not ok then return -2; end; "
        "    if b = null then return -1; end; "
        "    if b! = 200 then return 77; end; "
        "    return -9; "
        "  end Main; "
        "end C;"), 77);
}

// A floating value type (`double?`, {i8,double}): the strtod bridge marshals
// through the same helper.
BEZEL_TEST(generic_out_nullable_tryparse_1470, double_value_delivered) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var ok := U.TryIt<double>(\"1.5\", out var b); "
        "    if not ok then return -2; end; "
        "    if b = null then return -1; end; "
        "    if b! = 1.5 then return 33; end; "
        "    return -9; "
        "  end Main; "
        "end C;"), 33);
}

// A one-code-point value type (`char?`, {i8,i32}): IParsable<char> conformance.
BEZEL_TEST(generic_out_nullable_tryparse_1470, char_value_delivered) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var ok := U.TryIt<char>(\"A\", out var b); "
        "    if not ok then return -2; end; "
        "    if b = null then return -1; end; "
        "    if b! = 'A' then return 65; end; "
        "    return -9; "
        "  end Main; "
        "end C;"), 65);
}

// `decimal?` ({i8,decimal}): the runtime `bz_decimal_try_parse` writes a bare
// decimal, so the fix routes it through a scratch slot then marshals into the
// companion.
BEZEL_TEST(generic_out_nullable_tryparse_1470, decimal_value_delivered) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var ok := U.TryIt<decimal>(\"3.25\", out var b); "
        "    if not ok then return -2; end; "
        "    if b = null then return -1; end; "
        "    if b! = 3.25m then return 88; end; "
        "    return -9; "
        "  end Main; "
        "end C;"), 88);
}

// stdlib §2.19 / §1 shape (a): a FAILING parse returns false and the out reads
// as null
// (the v0.171 nullable-out convention: hasValue = the success bool).
BEZEL_TEST(generic_out_nullable_tryparse_1470, failed_parse_is_false_and_null) {
    BEZEL_EXPECT_EQ(run(
        std::string(kWrapper) +
        "class C "
        "  public static method Main(): int "
        "    var ok := U.TryIt<int>(\"notanumber\", out var b); "
        "    if ok then return -3; end; "
        "    if b <> null then return -4; end; "
        "    return 42; "
        "  end Main; "
        "end C;"), 42);
}

// The `var tmp: T?; T.TryParse(s, out tmp); v := tmp;` two-step variant from the
// issue — the nested write targets a LOCAL `T?` slot rather than a forwarded
// refParam, and must marshal identically.
BEZEL_TEST(generic_out_nullable_tryparse_1470, two_step_local_temp_variant) {
    // 12345 does not fit an 8-bit exit code — observe on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "static class U "
        "  public static method TryIt<T>(s: string, out v: T?): bool "
        "      where T: IParsable<T> "
        "    var tmp: T?; "
        "    var r := T.TryParse(s, out tmp); "
        "    v := tmp; "
        "    return r; "
        "  end TryIt; "
        "end U; "
        "class C "
        "  public static method Main(): int "
        "    var ok := U.TryIt<int>(\"12345\", out var b); "
        "    if not ok then return -2; end; "
        "    if b = null then return -1; end; "
        "    Console.WriteLine(b!); return 0; "
        "  end Main; "
        "end C;"), "12345");
}

// Regression: a DIRECT non-generic `int.TryParse(s, out var b)` — the concrete
// member's out is a bare `int` (stdlib §9.1), so the bare store path is
// exercised and must be unchanged by the fix.
BEZEL_TEST(generic_out_nullable_tryparse_1470, direct_int_tryparse_unaffected) {
    // 12345 does not fit an 8-bit exit code — observe on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    if int.TryParse(\"12345\", out var b) then "
        "      Console.WriteLine(b); return 0; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;"), "12345");
}
