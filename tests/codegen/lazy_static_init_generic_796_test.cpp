// fix #796 / §22.13 / §22.12 / §22.7: GENERIC types now initialize their static
// state LAZILY, on first use — not eagerly at program start. This is the generic
// analog of #720 (which made non-generic types lazy but left generics eager).
//
// A generic type has ONE per-type sfinit that initializes all of its
// instantiations at once (it stores each T-independent initializer into every
// per-instantiation global — emitStaticConstructorBody / #709). The lazy guard is
// therefore per-TYPE: first use of ANY instantiation runs that one sfinit once,
// and a static initializer that throws is observable AT THE ACCESS POINT (it puts
// the type in a permanent failed state and raises a catchable
// TypeInitializationException), rather than raising uncatchably at program start
// (the §22.13/§22.7 violation #720 fixed for non-generic types, here for generic).
//
// (Per-INSTANTIATION init-timing independence — only observable for T-DEPENDENT
// static initializers, which are not yet supported — remains a tracked follow-up.)
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

// §22.13/§22.7: a GENERIC type's static field initializer throws; the first
// access (inside a try) observes a catchable TypeInitializationException.
// Before this fix the throw was raised eagerly at program start (uncatchable →
// exit 70 "Unhandled exception").
BEZEL_TEST(lazy_static_init_generic_796, generic_field_init_exception_caught_at_access) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Boom<T> public static _x: int := C.Fail(); end Boom; "
        "class C "
        "  public static method Fail(): int => throw new Exception(\"boom\"); "
        "  public static method Main(): int "
        "    try "
        "      var v := Boom<int>._x; "
        "      return v; "
        "    catch Exception => return 42; "
        "    end; "
        "  end Main; "
        "end C;"), 42);
}

// §22.13: a throwing static initializer on a GENERIC type that is NEVER accessed
// must NOT run (lazy on first use) — the program completes normally. Before the
// fix the eager program-start init threw uncatchably even though the type is
// unused.
BEZEL_TEST(lazy_static_init_generic_796, generic_unused_type_never_initializes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Boom<T> public static _x: int := C.Fail(); end Boom; "
        "class C "
        "  public static method Fail(): int => throw new Exception(\"boom\"); "
        "  public static method Main(): int => 7; "
        "end C;"), 7);
}

// §22.12: each instantiation's static state is initialized (the one sfinit stores
// to every per-instantiation global) — both `Counter<int>._n` and
// `Counter<string>._n` read 5 after lazy first use. Pins that the lazy change did
// not disturb per-instantiation storage. 5 + 5 = 10.
BEZEL_TEST(lazy_static_init_generic_796, generic_per_instantiation_values_initialized) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> public static _n: int := C.Five(); end Counter; "
        "class C "
        "  public static method Five(): int => 5; "
        "  public static method Main(): int "
        "    return Counter<int>._n + Counter<string>._n; "  // 10
        "  end Main; "
        "end C;"), 10);
}

// §22.7: the permanent failed state — a SECOND access after a failed generic
// static init also raises a (fresh) TypeInitializationException, so both `try`s
// catch it. first=1 (first catch) then first+40 = 41 (second catch). A second
// access that escaped the failed state would return 50.
BEZEL_TEST(lazy_static_init_generic_796, generic_failed_state_reraises_on_second_access) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Boom<T> public static _x: int := C.Fail(); end Boom; "
        "class C "
        "  public static method Fail(): int => throw new Exception(\"boom\"); "
        "  public static method Main(): int "
        "    var first: int := 0; "
        "    try "
        "      var a := Boom<int>._x; "
        "    catch Exception => first := 1; "
        "    end; "
        "    try "
        "      var b := Boom<int>._x; "
        "      return 50; "
        "    catch Exception => return first + 40; "  // 41
        "    end; "
        "  end Main; "
        "end C;"), 41);
}

// §22.13: a generic type whose static state is reached only THROUGH a static
// method call (not a direct field access) also inits lazily on that first call —
// the static-method-entry emitEnsureStaticInit covers the generic owner. The
// method reads the lazily-initialized static and returns it. 9.
BEZEL_TEST(lazy_static_init_generic_796, generic_static_method_triggers_lazy_init) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Holder<T> "
        "  public static _v: int := C.Nine(); "
        "  public static method Get(): int => _v; "
        "end Holder; "
        "class C "
        "  public static method Nine(): int => 9; "
        "  public static method Main(): int => Holder<int>.Get(); "  // 9
        "end C;"), 9);
}
