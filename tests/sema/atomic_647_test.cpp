// #647 / stdlib §6.4 v0.140 — Atomic<T> and AtomicInteger<T> sema tests.
//
// Spec derivation:
//   stdlib §6.4 v0.140:
//     Atomic<T>: T restricted to {bool, char, byte, sbyte, short, ushort,
//       int, uint, long, ulong, IntPtr, UIntPtr}. Verbatim diagnostic:
//       "Atomic<T> requires a cell-sized primitive type; T is not eligible.
//        For floating-point atomics, use Atomic<int> with a bit-cast; for
//        record-typed atomics, use a Mutex."
//     AtomicInteger<T>: T restricted to {byte, sbyte, short, ushort, int,
//       uint, long, ulong}. bool/char/IntPtr/UIntPtr are NOT eligible.
//     Both types are sendable (§30.14): Shared<Atomic<T>> accepted (§28.13).
//     AtomicInteger<T>.Increment/Decrement/Add — available only on
//       AtomicInteger; calling them on Atomic<T> is a name-resolution error.
//
// Tests cover:
//   (a) Atomic<T> restriction — invalid T rejected with verbatim diagnostic.
//   (b) Atomic<T> restriction — valid T accepted (positive cases).
//   (c) AtomicInteger<T> restriction — invalid T rejected.
//   (d) AtomicInteger<T> restriction — valid T accepted.
//   (e) Atomic<T> method surface accepted: Value, Exchange, CompareExchange.
//   (f) AtomicInteger<T> method surface accepted: Value, Exchange,
//       CompareExchange, Increment, Decrement, Add.
//   (g) Increment/Decrement/Add on Atomic<T> rejected (not on surface).
//   (h) Unknown method on Atomic<T> rejected.
//   (i) Unknown method on AtomicInteger<T> rejected.
//   (j) Atomic<T> sendability: accepted as Domain.Spawn arg (§30.14).
//   (k) AtomicInteger<T> sendability: accepted as Domain.Spawn arg (§30.14).
//   (l) Channel<Atomic<int>> accepted (§30.15 / §3.6).
//   (m) Shared<Atomic<int>> sendability accepted (§28.13 v0.140).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiagContaining(const bezel::test::SemaResult& r, const std::string& substr) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(substr) != std::string::npos) return true;
    }
    return false;
}

bool hasNotSendableDiag(const bezel::test::SemaResult& r) {
    return hasDiagContaining(r, "sendable");
}

} // namespace

// ===========================================================================
// (a) Atomic<T> restriction — invalid T rejected (§6.4 verbatim diagnostic)
// ===========================================================================

// §6.4 v0.140: Atomic<float> is rejected — verbatim diagnostic fired.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_float_rejected_with_verbatim_diagnostic_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<float>(0.0); "
        "  end Test; "
        "end C;");
    // §6.4 verbatim: "Atomic<T> requires a cell-sized primitive type; T is not eligible."
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "Atomic<T> requires a cell-sized primitive type"));
}

// §6.4 v0.140: Atomic<double> is rejected.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_double_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<double>(0.0); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "Atomic<T> requires a cell-sized primitive type"));
}

// §6.4 v0.140: Atomic<string> is rejected.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_string_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<string>(\"\"); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "Atomic<T> requires a cell-sized primitive type"));
}

// ===========================================================================
// (b) Atomic<T> restriction — valid T accepted (positive cases)
// ===========================================================================

// §6.4 v0.140: Atomic<bool> is accepted.
BEZEL_TEST(atomic_647_sema_6_4, atomic_bool_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<bool>(false); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: Atomic<int> is accepted.
BEZEL_TEST(atomic_647_sema_6_4, atomic_int_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<int>(0); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: Atomic<long> is accepted.
BEZEL_TEST(atomic_647_sema_6_4, atomic_long_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<long>(0L); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: Atomic<byte> is accepted.
BEZEL_TEST(atomic_647_sema_6_4, atomic_byte_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<byte>(0); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: Atomic<char> is accepted.
BEZEL_TEST(atomic_647_sema_6_4, atomic_char_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<char>('A'); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (c) AtomicInteger<T> restriction — invalid T rejected
// ===========================================================================

// §6.4 v0.140: AtomicInteger<bool> is rejected (bool not in integer set).
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_integer_bool_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<bool>(false); "
        "  end Test; "
        "end C;");
    // §6.4: bool is not eligible for AtomicInteger.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "AtomicInteger<T> requires a primitive integer type"));
}

// §6.4 v0.140: AtomicInteger<char> is rejected.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_integer_char_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<char>('A'); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "AtomicInteger<T> requires a primitive integer type"));
}

// §6.4 v0.140: AtomicInteger<float> is rejected.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_integer_float_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<float>(0.0); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "AtomicInteger<T> requires a primitive integer type"));
}

// ===========================================================================
// (d) AtomicInteger<T> restriction — valid T accepted
// ===========================================================================

// §6.4 v0.140: AtomicInteger<int> is accepted.
BEZEL_TEST(atomic_647_sema_6_4, atomic_integer_int_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<int>(0); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: AtomicInteger<long> is accepted.
BEZEL_TEST(atomic_647_sema_6_4, atomic_integer_long_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<long>(0L); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: AtomicInteger<byte> is accepted.
BEZEL_TEST(atomic_647_sema_6_4, atomic_integer_byte_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<byte>(0); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (e) Atomic<T> method surface accepted: Value, Exchange, CompareExchange
// ===========================================================================

// §6.4 v0.140: Atomic<int>.Exchange() is a valid method — no error.
BEZEL_TEST(atomic_647_sema_6_4, atomic_exchange_method_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<int>(0); "
        "    var prev := a.Exchange(1); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: Atomic<int>.CompareExchange() is a valid method — no error.
BEZEL_TEST(atomic_647_sema_6_4, atomic_compare_exchange_method_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<int>(0); "
        "    var observed := a.CompareExchange(0, 1); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: Atomic<int>.Value property getter is accepted — no error.
BEZEL_TEST(atomic_647_sema_6_4, atomic_value_get_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<int>(42); "
        "    var v := a.Value; "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (f) AtomicInteger<T> method surface accepted
// ===========================================================================

// §6.4 v0.140: AtomicInteger<int>.Increment() is a valid method — no error.
BEZEL_TEST(atomic_647_sema_6_4, atomic_integer_increment_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<int>(0); "
        "    var v := a.Increment(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: AtomicInteger<int>.Decrement() is a valid method — no error.
BEZEL_TEST(atomic_647_sema_6_4, atomic_integer_decrement_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<int>(5); "
        "    var v := a.Decrement(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 v0.140: AtomicInteger<int>.Add(delta) is a valid method — no error.
BEZEL_TEST(atomic_647_sema_6_4, atomic_integer_add_accepted_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<int>(0); "
        "    var v := a.Add(10); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (g) Increment/Decrement/Add on Atomic<T> rejected (not on surface: §6.4)
// ===========================================================================

// §6.4: Atomic<int>.Increment() is NOT on the surface — rejected.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_increment_on_atomic_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<int>(0); "
        "    var v := a.Increment(); "
        "  end Test; "
        "end C;");
    // Increment is not on Atomic<T>'s §6.4 surface.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §6.4: Atomic<int>.Decrement() is NOT on the surface — rejected.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_decrement_on_atomic_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<int>(0); "
        "    var v := a.Decrement(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §6.4: Atomic<int>.Add(delta) is NOT on the surface — rejected.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_add_on_atomic_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<int>(0); "
        "    var v := a.Add(5); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (h) Unknown method on Atomic<T> rejected (§6.4 / #674)
// ===========================================================================

// §6.4 / #674: an unknown method on Atomic<T> produces a diagnostic.
BEZEL_TEST(atomic_647_sema_6_4, unknown_method_on_atomic_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new Atomic<int>(0); "
        "    a.Bogus(); "
        "  end Test; "
        "end C;");
    // 'Bogus' is not on Atomic's §6.4 surface.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (i) Unknown method on AtomicInteger<T> rejected (§6.4 / #674)
// ===========================================================================

// §6.4 / #674: an unknown method on AtomicInteger<T> produces a diagnostic.
BEZEL_TEST(atomic_647_sema_6_4, unknown_method_on_atomic_integer_rejected_6_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var a := new AtomicInteger<int>(0); "
        "    a.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (j) Atomic<T> sendability: accepted as Domain.Spawn arg (§30.14 / §6.4)
// ===========================================================================

// §30.14 / stdlib §6.4 v0.140: Atomic<int> passed as owned spawn argument → accepted.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_as_spawn_arg_accepted_6_4_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned a: Atomic<int>) "
        "    a.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var t := Domain.Spawn(Worker.Run, new Atomic<int>(0)); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Atomic<int> is sendable per §6.4 — no sendability error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ===========================================================================
// (k) AtomicInteger<T> sendability: accepted as Domain.Spawn arg (§30.14)
// ===========================================================================

// §30.14 / stdlib §6.4 v0.140: AtomicInteger<int> passed as owned spawn argument → accepted.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_integer_as_spawn_arg_accepted_6_4_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned a: AtomicInteger<int>) "
        "    a.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var t := Domain.Spawn(Worker.Run, new AtomicInteger<int>(0)); "
        "    await t; "
        "  end Go; "
        "end Main;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ===========================================================================
// (l) Channel<Atomic<int>> accepted (§30.15 / §3.6 / §6.4)
// ===========================================================================

// §30.15 / §3.6 / stdlib §6.4: Channel<Atomic<int>> accepted.
BEZEL_TEST(atomic_647_sema_6_4,
           channel_atomic_int_accepted_6_4_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Atomic<int>>(); "
        "  end Test; "
        "end C;");
    // Atomic<int> is sendable → Channel<Atomic<int>> is valid per §3.6/§30.15.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ===========================================================================
// (m) Shared<Atomic<int>> sendability accepted (§28.13 v0.140 / §6.4)
// ===========================================================================

// §28.13 v0.140 / §6.4: Shared<Atomic<int>> accepted as spawn arg (Atomic sendable).
// Mirrors the Shared<Mutex> pattern from shared_sync_sendable_689_test.cpp (a):
//   new Shared<T>(new T(...)) inline — ownership transfers directly, no using-var.
BEZEL_TEST(atomic_647_sema_6_4,
           shared_atomic_int_sendable_6_4_28_13) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned h: Shared<Atomic<int>>) "
        "    h.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var h := new Shared<Atomic<int>>(new Atomic<int>(0)); "
        "    using var t := Domain.Spawn(Worker.Run, move h); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Shared<Atomic<int>> is sendable per §28.13 v0.140 — no sendability error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ===========================================================================
// (n) Atomic<T> — null-declaration builtin skips where-T:value check (§6.4 / #4318)
// Before fix: no constraint check fired for null-declaration builtins.
// Fix adds kNullDeclValueConstraints to TypeResolution_Names.cpp, so the
// where-T:value gate now fires and rejects a class type argument.
// ===========================================================================

// §6.4 / #4318: Atomic<C> where C is a class is rejected — the where T: value
// constraint fires and diagnoses "does not satisfy the constraint `where T: value`".
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_class_type_rejected_value_constraint_s4318_001) {
    auto r = parseSemaAll(
        "module M; "
        "class C end C; "
        "class P "
        "  public static method Main(): int "
        "    var a := new Atomic<C>(new C()); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "does not satisfy the constraint `where T: value`"));
}

// §6.4 / #4318: AtomicInteger<C> where C is a class is rejected.
BEZEL_TEST(atomic_647_sema_6_4,
           atomicinteger_class_type_rejected_value_constraint_s4318_002) {
    auto r = parseSemaAll(
        "module M; "
        "class C end C; "
        "class P "
        "  public static method Main(): int "
        "    var a := new AtomicInteger<C>(new C()); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "does not satisfy the constraint `where T: value`"));
}

// §6.4 / #4318: Atomic<int> control — int satisfies where T: value, compiles.
BEZEL_TEST(atomic_647_sema_6_4,
           atomic_int_accepted_value_constraint_s4318_003) {
    auto r = parseSemaAll(
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var a := new Atomic<int>(0); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.4 / #4318: AtomicInteger<int> control — int satisfies where T: value.
BEZEL_TEST(atomic_647_sema_6_4,
           atomicinteger_int_accepted_value_constraint_s4318_004) {
    auto r = parseSemaAll(
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var a := new AtomicInteger<int>(0); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
