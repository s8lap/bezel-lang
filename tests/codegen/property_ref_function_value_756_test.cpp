// fix #756 / §27.6: a property reference used in function-value position
// (`var f: () -> int := b.Count`) produces the getter as a function value —
// equivalent to `() => b.Count`. Previously it silently compiled and SIGSEGV'd:
// the getter was evaluated immediately and the int was stored into a
// function-pointer slot, then called.
//
// Fix: sema target-types the property reference to the function (checkAssignable
// promotion at local-decl / assignment / return positions), and codegen emits a
// property thunk (makePropertyThunk) that captures the receiver (a borrow) and
// computes the getter on call — handling computed, block-form, virtual/interface
// (vtable/itable dispatch), and auto-property getters.
//
// NOTE: drives the real Driver (compileAndRun) — runJit does not crash-test
// these reliably. Receiver-lifetime escapes (returning/storing the thunk beyond
// the receiver's scope) are a pre-existing closure-capture gap shared with
// method references — tracked separately, not here.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace {
namespace fs = std::filesystem;

int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_756");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}
}  // namespace

// §27.6/#756: the issue's exact repro — computed (arrow) getter. `fn()` returns
// the getter value (7); previously SIGSEGV.
BEZEL_TEST(property_ref_function_value_756, computed_getter_repro) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int => _n; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(7); "
        "  var fn: () -> int := b.Count; "
        "  return fn(); "  // 7
        "end Main; end Program;"), 7);
}

// §27.6: a block-form getter (`get => do … end`) thunked.
BEZEL_TEST(property_ref_function_value_756, block_getter) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int get => do return _n + 1; end; end; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(7); "
        "  var fn: () -> int := b.Count; "
        "  return fn(); "  // 8
        "end Main; end Program;"), 8);
}

// §27.6: a VIRTUAL property reference dispatches DYNAMICALLY through the thunk —
// a base-typed receiver holding a derived instance calls the override's getter.
BEZEL_TEST(property_ref_function_value_756, virtual_property_dispatch) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Base public virtual property V: int => 1; destructor end destructor; end Base; "
        "class Derived: Base public override property V: int => 2; end Derived; "
        "class Program public static method Main(): int "
        "  using var d := new Derived(); "
        "  var bb: Base := d; "
        "  var fn: () -> int := bb.V; "
        "  return fn(); "  // 2 (Derived's getter)
        "end Main; end Program;"), 2);
}

// §27.6: an auto-property getter thunked.
BEZEL_TEST(property_ref_function_value_756, auto_property) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  public property Count: int; "
        "  public constructor(n: int) Count := n; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(6); "
        "  var fn: () -> int := b.Count; "
        "  return fn(); "  // 6
        "end Main; end Program;"), 6);
}

// §27.6 control: an ordinary value-position read is UNAFFECTED — `var n: int :=
// b.Count` still reads the property value (the promotion only fires for a
// function-typed target).
BEZEL_TEST(property_ref_function_value_756, value_position_unaffected) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int => _n; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(5); "
        "  var n: int := b.Count; "
        "  return n; "  // 5
        "end Main; end Program;"), 5);
}

// §27.6: the promotion also fires in ASSIGNMENT position (not just local-decl
// init) — `fn := b.Count` where fn is a function-typed local.
BEZEL_TEST(property_ref_function_value_756, assignment_position) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int => _n; "
        "  destructor end destructor; "
        "end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box(8); "
        "  var fn: () -> int := lambda() => 0; "
        "  fn := b.Count; "
        "  return fn(); "  // 8
        "end Main; end Program;"), 8);
}

// §23.1/§23.2 / §27.6 (#1187): reading a property whose DECLARED type is itself
// a function type is an ORDINARY value read — the getter already returns the
// function value. It must NOT be promoted by the #756 getter-as-function-value
// thunk (which would wrap it as `() -> ((int) -> int)` and bind that into the
// `(int) -> int` slot, calling it at the wrong arity). The static form
// previously SIGSEGV'd at the call. `f(10)` = 30.
BEZEL_TEST(property_ref_function_value_756, function_typed_property_read_static_1187) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static property Fn: (int) -> int => lambda(x: int) => x * 3; "
        "  public static method Main(): int "
        "    var f := C.Fn; "
        "    return f(10); "  // 30
        "  end Main; "
        "end C;"), 30);
}

// §23.1/§23.2 / §27.6 (#1187): the instance form of the above. The getter
// returns a fresh `(int) -> int` closure; reading `c.Fn` binds that function
// value directly (the instance form previously returned garbage). `f(10)` = 30.
BEZEL_TEST(property_ref_function_value_756, function_typed_property_read_instance_1187) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public property Fn: (int) -> int => lambda(x: int) => x * 3; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var f := c.Fn; "
        "    return f(10); "  // 30
        "  end Main; "
        "end C;"), 30);
}

// §27.6 / §7.4 (#1191): the getter-as-function-value promotion must ALSO fire in
// CALL-ARGUMENT position — `Apply(C.N)` passes property N's getter as the
// `() -> int` parameter. Sema lost the promotion context in argument position
// (checkAssignable was invoked without forwarding `fromExpr`), so the property
// read stayed an `int` value bound into a function-pointer slot and codegen
// emitted malformed IR ("Call parameter type does not match function
// signature"). With `fromExpr` forwarded at the call-arg site, the property
// reference is target-typed to `() -> int` exactly as at local-decl. `f()` = 7.
BEZEL_TEST(property_ref_function_value_756, function_value_in_call_argument_1191) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static property N: int => 7; "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method Main(): int => Apply(C.N); "  // 7
        "end C;"), 7);
}

// §27.6 / §7.4 (#1191): the instance-property form in call-argument position —
// `Apply(b.Count)` passes box b's getter as a `() -> int` argument.
BEZEL_TEST(property_ref_function_value_756, instance_property_value_in_call_argument_1191) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Count: int => _n; "
        "  destructor end destructor; "
        "end Box; "
        "class Program "
        "  public static method Apply(f: () -> int): int => f(); "
        "  public static method Main(): int "
        "    using var b := new Box(9); "
        "    return Apply(b.Count); "  // 9
        "  end Main; "
        "end Program;"), 9);
}

// fix #2501 (§27.6, v0.236): "the instance is captured at formation — by
// reference for class-typed receivers, by copy for value-shaped (record)
// receivers". Emitter::buildPropertyClosureCell captured the receiver into
// the closure env ONLY when it was already a POINTER (`receiverVal->
// getType()->isPointerTy()`); for a RECORD receiver (a by-value struct,
// §9.2/#347) that condition is false, so the env stayed the null-pointer
// default. makePropertyThunk's generated thunk body then unconditionally did
// `CreateLoad(ptrTy(), thunk->getArg(0))` to reconstruct `this` — a load
// through that null pointer — and the getter's implicit field read (`_pub`)
// GEPs off it, SIGSEGVing. This is the issue's own exact repro: a plain
// LOCAL record's arrow-getter property referenced as a function value.
// Before the fix: SIGSEGV (this whole gtest binary would have crashed,
// exactly as #756's own original bug did before its fix — see this file's
// header note). After the fix: `fn()` correctly returns 41.
BEZEL_TEST(property_ref_function_value_756, record_computed_getter_2501) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "public record RecThunkLocal(x: int) "
        "  public _pub: int := 41; "
        "  public property Getter: int => _pub; "
        "end RecThunkLocal; "
        "public class C "
        "  public static method Main(): int "
        "    var r := new RecThunkLocal(9); "
        "    var fn: () -> int := r.Getter; "
        "    return fn(); "  // 41
        "  end Main; "
        "end C;"), 41);
}

// fix #2501 (§27.6, v0.236): the SAME shape as above, but for a record's
// AUTO-property (isAutoPropertyStorage's makePropertyThunk branch — a
// DIFFERENT dispatch arm than the computed-getter branch the test above
// exercises: it GEPs the field directly off `thisVal` rather than lowering
// an expression body against `currentThis_`).
BEZEL_TEST(property_ref_function_value_756, record_auto_property_2501) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "public record RecAuto "
        "  public property Count: int; "
        "  public constructor(seed: int) Count := seed * 10; end constructor; "
        "end RecAuto; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := new RecAuto(4); "
        "    var fn: () -> int := r.Count; "
        "    return fn(); "  // 40
        "  end Main; "
        "end Program;"), 40);
}

// fix #2501 (§27.6, v0.236): a record's BLOCK-form getter (`get => do …
// end;`) thunked. Exercises a THIRD makePropertyThunk dispatch arm
// (blockGetterOf / ensurePropertyBlockGetter), whose generated function
// always takes `this` as a POINTER — the record receiver captured by copy
// must be re-spilled to a pointer for this call specifically (see the
// `blockThis` spill in makePropertyThunk).
BEZEL_TEST(property_ref_function_value_756, record_block_getter_2501) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "public record RecBlock(x: int) "
        "  public property Doubled: int get => do return x * 2; end; end; "
        "end RecBlock; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := new RecBlock(11); "
        "    var fn: () -> int := r.Doubled; "
        "    return fn(); "  // 22
        "  end Main; "
        "end Program;"), 22);
}

// fix #2501 (§27.6, v0.236): proves the spec's exact "by copy" wording, not
// merely "doesn't crash" — a record property reference must capture an
// INDEPENDENT COPY of the receiver at FORMATION time, not an alias of the
// local. `b` is REASSIGNED to a brand-new `Box` (a different `N`) AFTER
// `fn` is formed but BEFORE it is invoked; `fn()` must still observe the
// ORIGINAL `N` (21, doubled 42) — an aliasing (by-reference) implementation
// would observe the reassigned value (999, doubled 1998) instead.
BEZEL_TEST(property_ref_function_value_756, record_captured_by_copy_not_reference_2501) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "public record Box(N: int) "
        "  public property Doubled: int => N * 2; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box(21); "
        "    var fn: () -> int := b.Doubled; "
        "    b := new Box(999); "
        "    return fn(); "  // 42, NOT 1998
        "  end Main; "
        "end Program;"), 42);
}

// fix #2501 (§27.6, v0.236; #574 / §10.4 / §19.2 precedent): an ENUM
// receiver's env is likewise captured now (the raw scalar value, not a
// pointer). Separately, makePropertyThunk never set `currentEnumMethod_`
// around the computed-getter body it lowers, so an enum property whose
// getter dispatches with `case this ...` (the common enum-property idiom)
// was REJECTED at codegen — "the `case`/`when` pattern … did not resolve to
// a type, enum constant, or named constant" — even once the receiver-capture
// half of this fix landed; this test pins both halves together. Before
// either fix: a compile-time reject (not the record's SIGSEGV — the issue's
// own text notes the two receiver kinds fail differently pre-fix). After:
// `fn()` correctly dispatches on the captured Green constant, returning 2.
BEZEL_TEST(property_ref_function_value_756, enum_case_this_computed_getter_2501) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "enum Color Red, Green, Blue; "
        "  public property Code: int => "
        "    (case this of Red => 1, Green => 2, _ => 3); "
        "end Color; "
        "class App "
        "  public static method Main(): int "
        "    var c := Color.Green; "
        "    var fn: () -> int := c.Code; "
        "    return fn(); "  // 2
        "  end Main; "
        "end App;"), 2);
}
