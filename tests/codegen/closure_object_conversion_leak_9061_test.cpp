// Regression tests for #9061: a capture-free closure LOCAL converted to
// `Object` at a call argument (`reg.Keep(f)`) leaked its 64-byte closure cell
// (12800 B / 200 allocations over 200 loops), and so did a lambda literal at
// the same position and an `Object` local bound from the closure.
//
// WHICH REFERENCE WAS MISSING. Not the conversion's +1 and not the caller's
// disposal: the callee `Keep(o: Object) _o := o;` stores the `Object` view of
// the cell into an `Object?` FIELD, and that field held the cell WITHOUT a
// reference of its own and released nothing at disposal. The escape walk
// therefore had to call the argument an escape (the callee keeps it), so the
// caller never released its reference, and nobody ever did.
//
// THE RULE. §28.9 (v0.359): "Runtime management governs the *function value
// itself* -- the heap-allocated closure object that backs it"; §8.1: a
// function type reaches `Object` "via [its] reference-type heap
// representation". The slot that keeps the cell is therefore a holder of it
// and must hold a reference of its own (a run-time retain on a function value
// at every store into the field, a release of the displaced value and at the
// holder's disposal); the caller then keeps and releases its own at scope end.
//
// The rows run 200 loops on `runAotChecked` (ASan + leak check) with exact
// stdout, so they are RED on the base tree by the leak and also pin that the
// kept closure stays callable after the local's scope ends and after a second
// store (no use-after-free, no double release).
//
// per §28.9, §8.1, §27.5 (v0.359)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
template <class R>
std::string outOf(const R& r) {
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

std::string program(const std::string& body) {
    return std::string(R"BZL(
module M;
public class Reg2
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method KeepN(o: Object?) _o := o; end KeepN;
  public method Clear() _o := null; end Clear;
  public method Run(): int
    if _o is () -> int then return (_o as () -> int)!(); end if;
    return 7;
  end Run;
end Reg2;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
)BZL") + body + R"BZL(
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
)BZL";
}
}  // namespace

BEZEL_TEST(closure_object_conversion_leak_9061, issue_repro_local_closure_at_object_arg_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    reg.Keep(f);
    t := t + 1;
)BZL"))), "200\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, lambda_literal_at_object_arg_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    reg.Keep(lambda() => 9);
    t := t + reg.Run();
)BZL"))), "1800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, object_local_at_object_arg_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    var o: Object := f;
    reg.Keep(o);
    t := t + reg.Run();
)BZL"))), "1800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, stored_closure_invoked_after_keep_and_local_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    reg.Keep(f);
    t := t + f() + reg.Run();
)BZL"))), "3600\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, kept_twice_and_overwritten_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    var g: () -> int := lambda() => 4;
    reg.Keep(f);
    reg.Keep(f);
    reg.Keep(g);
    t := t + reg.Run();
)BZL"))), "800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, nullable_object_param_released_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    reg.KeepN(f);
    t := t + reg.Run();
)BZL"))), "1800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, local_dies_before_holder_still_callable_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    if true then
      var f: () -> int := lambda() => 9;
      reg.Keep(f);
    end if;
    t := t + reg.Run();
)BZL"))), "1800\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, one_closure_in_two_holders_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var r1 := new Reg2();
    var r2 := new Reg2();
    var f: () -> int := lambda() => 9;
    r1.Keep(f);
    r2.Keep(f);
    t := t + r1.Run() + r2.Run();
)BZL"))), "3600\n");
}

BEZEL_TEST(closure_object_conversion_leak_9061, cleared_holder_releases_its_reference_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(program(
R"BZL(    var reg := new Reg2();
    var f: () -> int := lambda() => 9;
    reg.Keep(f);
    reg.Clear();
    t := t + reg.Run();
)BZL"))), "1400\n");
}

// ---------------------------------------------------------------------------
// fix-forward #9061: the READ side. The rows above give the `Object?` field a
// counted reference (retain on store, release on displace and at the holder's
// disposal). A read that leaves the field as a value must then hold a
// reference of its own, as a function-typed field's getter already does, or
// the field's release frees the cell under the reader. Each row below makes
// the field the cell's ONLY holder (the helper's local is released at its
// scope end, or a lambda literal's +1 is transferred into the field) and then
// displaces the value or disposes the holder while a reader still uses it:
//   * an `Object?` getter bound to a local, then the field overwritten
//     (`h.Keep(new Foo(2))`) -- review probes i22 and j03;
//   * the getter's value returned out of a method whose holder dies before
//     the caller invokes it -- review probe j04;
//   * the closure invoked THROUGH the field, displacing it while it runs and
//     then reading its capture (the call's own operand is a reader too).
// Each was a heap-use-after-free under ASan on the first #9061 commit and a
// leak (no memory error) on the base; with the read side every row is clean.
//
// per §28.9 ("Runtime management governs the *function value itself*"),
// §8.1, §27.5 (v0.359)
namespace {
std::string holderProgram(const std::string& members,
                          const std::string& helpers,
                          const std::string& body) {
    return std::string(R"BZL(
module M;
public class Foo
  public property V: int;
  public constructor(v: int) V := v; end constructor;
end Foo;
public class Hx
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
)BZL") + members + R"BZL(end Hx;
public class H
)BZL" + helpers + R"BZL(end H;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
)BZL" + body + R"BZL(
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
)BZL";
}
}  // namespace

// Review probe i22_get_then_overwrite_sole_holder.
BEZEL_TEST(closure_object_conversion_leak_9061, getter_local_survives_overwrite_of_sole_holder_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(holderProgram(
R"BZL(  public method Get(): Object? => _o;
)BZL",
R"BZL(  public static method Fill(r: Hx) var f: () -> int := lambda() => 9; r.Keep(f); end Fill;
)BZL",
R"BZL(      var h := new Hx(); H.Fill(h); var g := h.Get(); h.Keep(new Foo(2));
      if g is () -> int then t := t + (g as () -> int)!(); end if;
)BZL"))), "1800\n");
}

// Review probe j03_lambda_fill_get_overwrite.
BEZEL_TEST(closure_object_conversion_leak_9061, lambda_filled_field_getter_survives_overwrite_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(holderProgram(
R"BZL(  public method Fill() _o := lambda() => 9; end Fill;
  public method Get(): Object? => _o;
)BZL",
"",
R"BZL(      var h := new Hx(); h.Fill(); var g := h.Get(); h.Keep(new Foo(2));
      if g is () -> int then t := t + (g as () -> int)!(); end if;
)BZL"))), "1800\n");
}

// Review probe j04_get_then_holder_dies.
BEZEL_TEST(closure_object_conversion_leak_9061, getter_value_returned_past_holder_disposal_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(holderProgram(
R"BZL(  public method Get(): Object? => _o;
)BZL",
R"BZL(  public static method Fill(r: Hx) var f: () -> int := lambda() => 9; r.Keep(f); end Fill;
  public static method Grab(): Object? var h := new Hx(); H.Fill(h); return h.Get(); end Grab;
)BZL",
R"BZL(      var g := H.Grab();
      if g is () -> int then t := t + (g as () -> int)!(); end if;
)BZL"))), "1800\n");
}

// The invocation through the field is a reader: the closure displaces the
// field (its only holder) while it runs, then reads its captured `k`.
BEZEL_TEST(closure_object_conversion_leak_9061, closure_invoked_through_field_displaces_it_while_running_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(holderProgram(
R"BZL(  public method Fill(k: int) _o := lambda(x: Hx) => do x.Keep(new Foo(1)); return k + 1; end; end Fill;
  public method Run(): int
    if _o is (Hx) -> int then return (_o as (Hx) -> int)!(this); end if;
    return 7;
  end Run;
)BZL",
"",
R"BZL(      var h := new Hx(); h.Fill(i mod 3); t := t + h.Run();
)BZL"))), "399\n");
}

// The same, with the field filled from a helper's released closure local.
BEZEL_TEST(closure_object_conversion_leak_9061, kept_closure_invoked_through_field_displaces_it_while_running_spec_28_9) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(holderProgram(
R"BZL(  public method Run(): int
    if _o is (Hx) -> int then return (_o as (Hx) -> int)!(this); end if;
    return 7;
  end Run;
)BZL",
R"BZL(  public static method Fill(r: Hx, k: int)
    var f: (Hx) -> int := lambda(x: Hx) => do x.Keep(new Foo(1)); return k + 1; end;
    r.Keep(f);
  end Fill;
)BZL",
R"BZL(      var h := new Hx(); H.Fill(h, i mod 3); t := t + h.Run();
)BZL"))), "399\n");
}

// §27.5 keeps a closure's captures alive as long as the closure: a capturing
// closure local a helper hands to the field outlives the helper's frame, so
// the cell of the captured LOCAL `j` must not be freed at the helper's end.
// (Probe q02; the first #9061 commit freed it there, a heap-use-after-free on
// the first `Run`.)
BEZEL_TEST(closure_object_conversion_leak_9061, captured_local_cell_outlives_helper_that_kept_closure_spec_27_5) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(holderProgram(
R"BZL(  public method Run(): int
    if _o is () -> int then return (_o as () -> int)!(); end if;
    return 7;
  end Run;
)BZL",
R"BZL(  public static method FillL(r: Hx, k: int)
    var j := k * 2;
    var f: () -> int := lambda() => j + 1;
    r.Keep(f);
  end FillL;
)BZL",
R"BZL(      var h := new Hx(); H.FillL(h, i mod 3); t := t + h.Run();
)BZL"))), "598\n");
}

// The same, handed on through an `Object` alias of the closure local (probe
// q04): the alias carries the value into the field, so the source local's
// captures outlive the helper as well.
BEZEL_TEST(closure_object_conversion_leak_9061, captured_local_cell_outlives_helper_object_alias_kept_spec_27_5) {
    BEZEL_EXPECT_EQ(outOf(runAotChecked(holderProgram(
R"BZL(  public method Run(): int
    if _o is () -> int then return (_o as () -> int)!(); end if;
    return 7;
  end Run;
)BZL",
R"BZL(  public static method FillL(r: Hx, k: int)
    var j := k * 2;
    var f: () -> int := lambda() => j + 1;
    var o: Object := f;
    r.Keep(o);
  end FillL;
)BZL",
R"BZL(      var h := new Hx(); H.FillL(h, i mod 3); t := t + h.Run();
)BZL"))), "598\n");
}
