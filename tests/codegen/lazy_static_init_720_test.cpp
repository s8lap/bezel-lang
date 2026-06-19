// fix #720 (B3) / §22.13 / §22.7: NON-GENERIC types initialize their static
// state LAZILY, on first use — not eagerly at program start. A static
// initializer (field initializer or static constructor body) that throws an
// uncaught exception is observable AT THE ACCESS POINT: it puts the type in a
// permanent failed state and raises TypeInitializationException (a subclass of
// Exception, InnerException = the original) at that access and every subsequent
// one, so a surrounding `try`/`catch` can handle it.
//
// (Generic types keep eager program-start init — their per-instantiation lazy
// init is a tracked follow-up; not covered here.)
//
// These are codegen tests (static-init timing + exception routing), so runJit is
// the right harness — exit codes capture both returned values and caught-exception
// outcomes.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §22.13/§22.7: the canonical #720-B3 repro — a static field initializer throws;
// the first access (inside a try) observes a catchable TypeInitializationException.
// Before the fix this was raised eagerly at program start (uncatchable → exit 70).
BEZEL_TEST(lazy_static_init_720, field_init_exception_caught_at_access) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Boom public static _x: int := C.Fail(); end Boom; "
        "class C "
        "  public static method Fail(): int => throw new Exception(\"boom\"); "
        "  public static method Main(): int "
        "    try "
        "      var v := Boom._x; "
        "      return v; "
        "    catch Exception => return 42; "
        "    end; "
        "  end Main; "
        "end C;"), 42);
}

// §22.13: a throwing static initializer on a type that is NEVER used must NOT run
// (lazy on first use) — the program completes normally.
BEZEL_TEST(lazy_static_init_720, unused_type_never_initializes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Boom public static _x: int := C.Fail(); end Boom; "
        "class C "
        "  public static method Fail(): int => throw new Exception(\"boom\"); "
        "  public static method Main(): int => 7; "
        "end C;"), 7);
}

// §22.7: the permanent failed state — a SECOND access after a failed init also
// raises a (fresh) TypeInitializationException, so both `try`s catch it. (Pins
// that re-raise does not reuse the reclaimed first exception block.)
BEZEL_TEST(lazy_static_init_720, failed_state_persists_across_accesses) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Boom public static _x: int := C.Fail(); end Boom; "
        "class C "
        "  public static method Fail(): int => throw new Exception(\"boom\"); "
        "  public static method Main(): int "
        "    var first := 0; "
        "    try var a := Boom._x; catch Exception => first := 1; end; "
        "    var second := 0; "
        "    try var b := Boom._x; catch Exception => second := 1; end; "
        "    return first * 10 + second; "      // 11
        "  end Main; "
        "end C;"), 11);
}

// §22.13: writing a static field is a first use — it must run the initializer
// FIRST, then apply the write (so the write is not clobbered by a later lazy
// initializer running on the subsequent read).
BEZEL_TEST(lazy_static_init_720, write_before_read_not_clobbered) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class S "
        "  public static _v: int := Seed(); "
        "  public static method Seed(): int => 5; "
        "end S; "
        "class C "
        "  public static method Main(): int "
        "    S._v := 100; "
        "    return S._v; "                      // 100, not 5
        "  end Main; "
        "end C;"), 100);
}

// §22.13: "before any of its methods can run" — calling a static method triggers
// the declaring type's init even when the method reads no static field. Here the
// static constructor's side effect is observed only because Ping() ran the init.
BEZEL_TEST(lazy_static_init_720, static_method_call_triggers_cctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Tracker public static _count: int := 0; end Tracker; "
        "class Boom "
        "  static constructor() Tracker._count := 99; end constructor; "
        "  public static method Ping(): int => 5; "
        "end Boom; "
        "class C "
        "  public static method Main(): int "
        "    var p := Boom.Ping(); "
        "    return Tracker._count; "            // 99 (cctor ran on the Ping() call)
        "  end Main; "
        "end C;"), 99);
}

// §22.13: a static AUTO-property (backing global) initializer that throws is also
// observed lazily at the access point.
BEZEL_TEST(lazy_static_init_720, static_auto_property_init_caught) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Reg public static Slot: int := C.Fail(); end Reg; "
        "class C "
        "  public static method Fail(): int => throw new Exception(\"x\"); "
        "  public static method Main(): int "
        "    try return Reg.Slot; catch Exception => return 33; end; "
        "  end Main; "
        "end C;"), 33);
}

// §22.13: a successful lazy init returns the initialized value; a static computed
// property reading another static member sees it initialized.
BEZEL_TEST(lazy_static_init_720, successful_init_value_observed) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Reg "
        "  public static Slot: int := Seed(); "
        "  public static method Seed(): int => 11; "
        "  public static property Doubled: int => Slot * 2; "
        "end Reg; "
        "class C "
        "  public static method Main(): int => Reg.Doubled; "  // 22
        "end C;"), 22);
}
