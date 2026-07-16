// Codegen — issues #1894 / #1895 (§14.1 v0.184; §2.8.3 rule 2 v0.186; §9.4
// record field access): a generic METHOD that returns a generic RECORD
// instantiated at `long` (i64) fields returned a CORRUPTED value — a silent
// wrong-value miscompile (and, under ASan, a stack-buffer-overflow on the
// caller's return slot).
//
// THE BUG: `applyMethodGenericSubst` bound each method type parameter by
// inferring it from the ARGUMENT's own expression type BEFORE consulting the
// explicit call-site type arguments, and the explicit-type-arg pass then SKIPPED
// any already-bound parameter. For `Maker.Make<long, long>(100, 200)` the int
// literals `100`/`200` are typed `int` by sema, so inference bound A/B → int
// (i32) and the explicit `<long, long>` was ignored. The generic method was then
// monomorphized at the i32-placeholder record layout ({i32,i32}, 8 bytes) while
// the caller — which reads `p.First`/`p.Second` through `p`'s CONCRETE sema type
// `Pair<long,long>` — used the i64 layout ({i64,i64}, 16 bytes). The two
// disagreed across the return-value boundary: the returned bytes decoded as an
// i32-pair scramble ((200<<32)|100 = 858993459300) and an i64 field read ran off
// the end of the 8-byte return slot. Per §14.1 / §2.8.3 rule 2 the monomorph-
// ization MUST use the substituted-type layout, so the explicit type arguments
// are authoritative.
//
// #1894 is the register-return case (Pair<long,long> ≤ 16 bytes, returned in
// RAX:RDX); #1895 is the memory/sret case (Triple<long,long,long> = 24 bytes,
// returned via a hidden pointer). They share the root cause but exercise
// DIFFERENT return-ABI paths, so both are pinned here.
//
// THE FIX (LlvmBackend_Calls2.cpp applyMethodGenericSubst): bind the EXPLICIT
// call-site type arguments FIRST — since the argument-inference helper skips any
// already-bound param, the explicit `<long, long>` now wins (A/B → i64). A
// second, latent defect this exposed: the specialized-function cache keyed only
// on (method, substitution, type-identity), so `Make<long,long>(100,200)` (i32
// literal args) and `Make<long,long>(x,y)` (i64 variable args) collided — the
// second reused the first's `(i32,i32)` function and passed it i64 args
// (IR-verify failure). The cache key now also includes the concrete LLVM
// parameter-type vector, minting one correctly-typed specialization per
// signature.
//
// runAotChecked instruments the generated code with AddressSanitizer, so the
// pre-fix stack-buffer-overflow on the return slot makes `r.ran` false; the
// stdout/exit assertions additionally pin the CONCRETE field values, so a
// silent wrong-value regression (no crash) is caught even without ASan.

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// #1894 — register-return case: `Maker.Make<long,long>(100,200)` returns a
// Pair<long,long> whose fields must read First=100, Second=200 (§9.4); the
// program prints "100\n200\n" and returns int(100+200)=300 (exit 300&0xFF=44).
BEZEL_TEST(generic_method_record_return_wide_field_1894_1895,
           pair_long_long_register_return_1894) {
    auto r = runAotChecked(
        "module M; "
        "record Pair<A, B>(First: A, Second: B); "
        "class Maker "
        "  public static method Make<A, B>(a: A, b: B): Pair<A, B> "
        "      where A: value where B: value "
        "    return new Pair<A, B>(a, b); "
        "  end Make; "
        "end Maker; "
        "class Program "
        "  public static method Main(): int "
        "    var p := Maker.Make<long, long>(100, 200); "
        "    Console.WriteLine(p.First.ToString()); "
        "    Console.WriteLine(p.Second.ToString()); "
        "    return int(p.First + p.Second); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);            // no ASan stack-buffer-overflow
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n200\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 44);     // int(100+200)=300, 300 & 0xFF
}

// #1895 — memory/sret-return case: `Maker.Make<long,long,long>(10,20,30)`
// returns a 24-byte Triple<long,long,long> (> 16 bytes → returned via a hidden
// sret pointer, a DIFFERENT ABI path from the register case above). Fields must
// read X=10, Y=20, Z=30 (§9.4); the program prints "10\n20\n30\n".
BEZEL_TEST(generic_method_record_return_wide_field_1894_1895,
           triple_long_long_long_sret_return_1895) {
    auto r = runAotChecked(
        "module M; "
        "record Triple<A, B, C>(X: A, Y: B, Z: C); "
        "class Maker "
        "  public static method Make<A, B, C>(a: A, b: B, c: C): Triple<A, B, C> "
        "      where A: value where B: value where C: value "
        "    return new Triple<A, B, C>(a, b, c); "
        "  end Make; "
        "end Maker; "
        "class Program "
        "  public static method Main(): int "
        "    var t := Maker.Make<long, long, long>(10, 20, 30); "
        "    Console.WriteLine(t.X.ToString()); "
        "    Console.WriteLine(t.Y.ToString()); "
        "    Console.WriteLine(t.Z.ToString()); "
        "    return int(t.X + t.Y + t.Z); "  // 60 → exit 60
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);            // no ASan stack-buffer-overflow
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n20\n30\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// Cache-collision regression (the second, latent defect the primary fix
// exposed): the SAME generic instantiation `Make<long,long>` reached from two
// call sites — one with int-literal args (lowered i32), one with `long`-variable
// args (lowered i64) — must produce two correctly-typed specializations, not one
// shared function called at two different argument widths (which IR-verify would
// reject). `lit.First`=100, `vr.First`=7, `vr.Second`=8; exit int(100+8)=108.
BEZEL_TEST(generic_method_record_return_wide_field_1894_1895,
           mixed_literal_and_variable_arg_widths_same_type_args) {
    auto r = runAotChecked(
        "module M; "
        "record Pair<A, B>(First: A, Second: B); "
        "class Maker "
        "  public static method Make<A, B>(a: A, b: B): Pair<A, B> "
        "      where A: value where B: value "
        "    return new Pair<A, B>(a, b); "
        "  end Make; "
        "end Maker; "
        "class Program "
        "  public static method Main(): int "
        "    var lit := Maker.Make<long, long>(100, 200); "
        "    var x: long := 7; var y: long := 8; "
        "    var vr := Maker.Make<long, long>(x, y); "
        "    Console.WriteLine(lit.First.ToString()); "
        "    Console.WriteLine(vr.First.ToString()); "
        "    Console.WriteLine(vr.Second.ToString()); "
        "    return int(lit.First + vr.Second); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n7\n8\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 108);
}

// Mixed-width unsigned-field control: a Pair<ulong, int> ({i64, i32}) built by
// the generic method round-trips a full 2^32 first field exactly — confirms the
// explicit-type-arg-precedence change does not regress a heterogeneous-width
// unsigned instantiation returned from a generic method. The 2^32 value does not
// fit an 8-bit exit code, so it is checked on stdout.
BEZEL_TEST(generic_method_record_return_wide_field_1894_1895,
           ulong_int_mixed_width_fields_round_trip) {
    auto r = runAotChecked(
        "module M; "
        "record Pair<A, B>(First: A, Second: B); "
        "class Maker "
        "  public static method Make<A, B>(a: A, b: B): Pair<A, B> "
        "      where A: value where B: value "
        "    return new Pair<A, B>(a, b); "
        "  end Make; "
        "end Maker; "
        "class Program "
        "  public static method Main(): int "
        "    var big: ulong := 4294967296; "  // 2^32
        "    var p := Maker.Make<ulong, int>(big, 5); "
        "    Console.WriteLine(p.First.ToString()); "
        "    Console.WriteLine(p.Second.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4294967296\n5\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Int control (the layout that COINCIDENTALLY matched the i32 placeholder, so it
// read correctly even before the fix): must keep working — the explicit-type-arg
// precedence change does not perturb the int instantiation. Prints "3\n4\n",
// exit int(3+4)=7.
BEZEL_TEST(generic_method_record_return_wide_field_1894_1895,
           int_int_control_still_correct) {
    auto r = runAotChecked(
        "module M; "
        "record Pair<A, B>(First: A, Second: B); "
        "class Maker "
        "  public static method Make<A, B>(a: A, b: B): Pair<A, B> "
        "      where A: value where B: value "
        "    return new Pair<A, B>(a, b); "
        "  end Make; "
        "end Maker; "
        "class Program "
        "  public static method Main(): int "
        "    var p := Maker.Make<int, int>(3, 4); "
        "    Console.WriteLine(p.First.ToString()); "
        "    Console.WriteLine(p.Second.ToString()); "
        "    return int(p.First + p.Second); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n4\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
