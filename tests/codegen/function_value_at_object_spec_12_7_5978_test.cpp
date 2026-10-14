// Regression pins for issue #5978 (spec §8.1 "Definition" of `Object`, §12.7
// "Type tests", §8.3 / §27.7.1 "Equality of function values", §28.3
// *Set-erasing positions* clause 4; all v0.338): a function value held at
// `Object`.
//
// §8.1: "`string`, interfaces, and function types reaching `Object` similarly
// via their reference-type heap representation" — a function value at `Object`
// is an ordinary object. Before the fix the `Object` slot held the bare
// closure cell, whose word 0 is the code pointer, so every `Object` operation
// read a code pointer where it expected a vtable: `o is string` answered
// `True`, `o as string` was non-null, and `o.ToString()` released the cell as
// a string (SEGV in `bz_string_release` under ASan). Each row below prints a
// value the pre-fix compiler gets wrong or crashes on.
//
// ORACLES. A function value that is passed on from an `Object` location (as
// a call argument or a receiver), copied from a function-typed local, or
// overwritten in its slot is not reclaimed: the cell leaks, one per
// evaluation. That is the pre-existing #5967-family leak, present before this
// fix with the identical allocation count, and not this file's subject. So
// the value rows run on `runAot`, and one row
// (`direct_object_local_type_tests_are_sanitizer_clean_12_7`) keeps to the
// shapes that ARE reclaimed and runs looped on `runAotChecked` (generated-
// code ASan + LSan), where a wrong release of the new `Object` view — a
// double free or a free of an interior pointer — would be reported. Under
// `BEZEL_AOT_FORCE_SANITIZE=address` the `runAot` rows also run with ASan's
// memory-error checks armed.
//
// `ToString()` on a function value has no §8.5 bullet; the rows assert only
// that it yields a non-empty string and does not crash, never its text.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kDecls =
    "module M; "
    "public class Animal "
    "  public constructor() end constructor; "
    "end Animal; "
    "public interface IFoo "
    "  method Foo(): int; "
    "end IFoo; "
    "public class Acc "
    "  public _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method Add(x: int): int => x + _n; "
    "end Acc; "
    "public record Box(V: Object); "
    "public choice Ch "
    "  Some(v: Object), "
    "  None; "
    "end Ch; "
    "public static class Util "
    "  public static method Twice(n: int): int => n * 2; "
    "  public static method Thrice(n: int): int => n * 3; "
    "  public static method Pass<T>(x: T): T => x; "
    "  public static method IsStr<T>(x: T): bool => x is string; "
    "  public static method Un(c: Ch): Object => "
    "    (case c of Some(v) => v, None => \"none\"); "
    "  public static method Kind(o: Object): string => "
    "    (case o of "
    "       var s: string => \"string\", "
    "       var i: int => \"int\", "
    "       var a: Animal => \"animal\", "
    "       _ => \"other\"); "
    "end Util; ";

// The four non-function probes of one `Object` value `o`, one line.
constexpr const char* kProbe =
    "    Console.WriteLine($\"{o is string} {o is Animal} {o is IFoo} "
    "{o is int} {(o as string) = null} {(o as Animal) = null} "
    "{o.ToString().Length > 0} {Util.Kind(o)}\"); ";

constexpr const char* kProbeExpected =
    "False False False False True True True other\n";

std::string mainWith(const std::string& body) {
    return std::string(kDecls) +
           "public class Program "
           "  public static method Main() " + body +
           "  end Main; "
           "end Program;";
}

}  // namespace

// The issue's own repro, verbatim in substance (the lambda spelled with the
// `lambda` keyword §27.3 requires).
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          issue_repro_is_string_false_and_tostring_does_not_crash_12_7) {
    const std::string src = mainWith(
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    var o: Object := f; "
        "    Console.WriteLine(o is string); "
        "    Console.WriteLine(o.ToString().Length > 0); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nTrue\n"));
}

// §12.7: every source kind of function value, placed at `Object?` and read
// back — not a string, class, interface or boxed value; `as` yields null.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          every_source_kind_is_no_string_class_interface_or_box_12_7) {
    const std::string src = mainWith(
        "    var k: int := 5; "
        "    var i: int := 0; "
        "    while i < 60 do "
        "      var f1: (int) -> int := Util.Twice; "
        "      var f2: (int) -> int := lambda(x: int) => x + k + 1; "
        "      var f3: (int) -> int := lambda(x: int) => x + 1; "
        "      var f4: (int) -> int := lambda(x: int) => x + k; "
        "      var n1: Object? := f1; "
        "      var n2: Object? := f2; "
        "      var n3: Object? := f3; "
        "      var n4: Object? := f4; "
        "      if n1 = null or n2 = null or n3 = null or n4 = null then "
        "        return; "
        "      end; "
        "      if i = 59 then "
        "        var o: Object := n1!; " + std::string(kProbe) +
        "        o := n2!; " + kProbe +
        "        o := n3!; " + kProbe +
        "        o := n4!; " + kProbe +
        "      end; "
        "      i := i + 1; "
        "    end; ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string(kProbeExpected) + kProbeExpected +
                        kProbeExpected + kProbeExpected);
}

// §8.1: the conversion is the same at every `Object` location — a local, a
// positional record field, a choice payload, a generic `T := Object`
// argument, an `Array<Object>` element, an explicit `as Object`.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          every_object_location_holds_an_ordinary_object_8_1) {
    const std::string src = mainWith(
        "    var f: (int) -> int := Util.Twice; "
        "    var o: Object := f; " + std::string(kProbe) +
        "    var b := new Box(f); "
        "    o := b.V; " + kProbe +
        "    var c := Ch.Some(f); "
        "    o := Util.Un(c); " + kProbe +
        "    o := Util.Pass<Object>(f); " + kProbe +
        "    var arr: Array<Object> := [f, \"s\", 5]; "
        "    o := arr[0]; " + kProbe +
        "    Console.WriteLine($\"{Util.Kind(arr[1])} {Util.Kind(arr[2])}\"); "
        "    var q := f as Object; "
        "    if q <> null then o := q!; " + kProbe + " end; "
        "    Console.WriteLine(f is Object); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int i = 0; i < 5; ++i) expected += kProbeExpected;
    expected += "string int\n";
    expected += kProbeExpected;
    expected += "True\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// §12.7: a FUNCTION-typed operand — here a `T` bound to a function type —
// tested against a non-function target. Pre-fix `x is string` answered `True`.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          function_typed_type_parameter_operand_is_no_string_12_7) {
    const std::string src = mainWith(
        "    var f: (int) -> int := Util.Twice; "
        "    Console.WriteLine(Util.IsStr<(int) -> int>(f)); "
        "    Console.WriteLine(Util.IsStr<string>(\"s\")); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nTrue\n"));
}

// §27.7.1 (v0.338): a function value's `GetHashCode` "agrees with this
// equality", and the hash "holds at `Object`, where a capture-free function
// value is admitted (§28.3's *Set-erasing positions*) and is an ordinary
// key". So at `Object` a static reference equals every reference to the same
// method; a class-receiver reference equals one over the same receiver; a
// lambda equals only itself and never a method reference; and the hash
// agrees with equality. The class-receiver reference is compared at its
// function type (`=`), not at `Object`: an instance method reference at
// `Object` is §28.3 *Set-erasing positions* (v0.338) invalid (#6215, #6261).
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          equals_and_hash_at_object_follow_function_value_equality_27_7_1) {
    const std::string src = mainWith(
        "    using var acc := new Acc(3); "
        "    using var acc2 := new Acc(3); "
        "    var good: int := 0; "
        "    var i: int := 0; "
        "    while i < 80 do "
        "      var s1: (int) -> int := Util.Twice; "
        "      var s2: (int) -> int := Util.Twice; "
        "      var s3: (int) -> int := Util.Thrice; "
        "      var m1: (int) -> int := acc.Add; "
        "      var m2: (int) -> int := acc.Add; "
        "      var m3: (int) -> int := acc2.Add; "
        "      var l1: (int) -> int := lambda(x: int) => x + 1; "
        "      var l2: (int) -> int := lambda(x: int) => x + 1; "
        "      var os1: Object? := s1; var os2: Object? := s2; "
        "      var os3: Object? := s3; "
        "      var ol1: Object? := l1; var ol2: Object? := l2; "
        "      if os1 = null or os2 = null or os3 = null or "
        "         ol1 = null or ol2 = null then "
        "        return; "
        "      end; "
        "      if os1!.Equals(os2!) and "
        "         os1!.GetHashCode() = os2!.GetHashCode() then "
        "        good := good + 1; end; "
        "      if not os1!.Equals(os3!) then good := good + 1; end; "
        "      if m1 = m2 and m1.GetHashCode() = m2.GetHashCode() then "
        "        good := good + 1; end; "
        "      if m1 <> m3 then good := good + 1; end; "
        "      if ol1!.Equals(ol1!) and "
        "         ol1!.GetHashCode() = ol1!.GetHashCode() then "
        "        good := good + 1; end; "
        "      if not ol1!.Equals(ol2!) then good := good + 1; end; "
        "      if not ol1!.Equals(os1!) then good := good + 1; end; "
        "      if not os1!.Equals(\"Twice\") then good := good + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(good); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("640\n"));   // 8 * 80
}

// §27.7.1 (v0.338): at `Object` a function value "is an ordinary key" —
// `Dictionary<Object, int>` finds a static reference through a SEPARATE
// evaluation and a class-receiver reference through a separate evaluation
// over the same receiver.
// PENDING #6215: `var o2: Object := acc.Add` is a receiver-bearing method
// reference at an `Object` initializer, which §28.3 *Set-erasing positions*
// (v0.338) rejects; the compiler still accepts it. No spelling keeps the
// class-receiver key (a function type is not hashable, §27.7.1), so the row
// is renamed, not re-spelled; flip it when #6215 lands.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          function_value_is_an_ordinary_object_key_27_7_1_pending_6215) {
    const std::string src = std::string(kDecls) +
        "public class Program "
        "  public static method MakeStatic(): Object => Util.Twice; "
        "  public static method Main() "
        "    using var acc := new Acc(3); "
        "    var good: int := 0; "
        "    var i: int := 0; "
        "    while i < 60 do "
        "      var o1: Object := MakeStatic(); "
        "      var o2: Object := acc.Add; "
        "      var o3: Object := acc.Add; "
        "      var d := new Dictionary<Object, int>(); "
        "      d[o1] := 1; "
        "      d[o2] := 2; "
        "      d[\"k\"] := 3; "
        "      if d[MakeStatic()] = 1 and d[o3] = 2 and d[\"k\"] = 3 then "
        "        good := good + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(good); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("60\n"));
}

// §28.3 Set-erasing positions clause 4 / §8.1: the value keeps working as a
// FUNCTION after it has also been viewed as an `Object`, and an `Object`
// slot can be re-pointed between a function value, a string and a box.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          function_stays_invocable_and_slot_is_reassignable_8_1) {
    const std::string src = mainWith(
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 80 do "
        "      var f: (int) -> int := lambda(x: int) => x + i; "
        "      var n: Object? := f; "
        "      if n <> null and not (n! is string) then "
        "        total := total + f(1) - i; "
        "      end; "
        "      n := \"s\"; "
        "      if n! is string then total := total + 1; end; "
        "      n := f; "
        "      if not (n! is string) then total := total + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(total); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("240\n"));   // 3 * 80
}

// §8.1 / §12.7 under the generated-code sanitizer: the shapes whose cell IS
// reclaimed (an `Object` local initialised directly from a static method
// reference or a lambda, and an explicit `as Object` view of a live function
// local), looped. The `Object` reference is an interior view of the function
// object; releasing it must free the object exactly once.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          direct_object_local_type_tests_are_sanitizer_clean_12_7) {
    const std::string src = mainWith(
        "    var good: int := 0; "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      var o1: Object := Util.Twice; "
        "      var o2: Object := lambda(x: int) => x + 1; "
        "      var f: (int) -> int := lambda(x: int) => x + 2; "
        "      var q := f as Object; "
        "      if not (o1 is string) and not (o1 is Animal) and "
        "         not (o1 is IFoo) and not (o1 is int) and "
        "         (o1 as string) = null and (o1 as Animal) = null then "
        "        good := good + 1; "
        "      end; "
        "      if not (o2 is string) and not (o2 is Animal) and "
        "         not (o2 is IFoo) and not (o2 is int) and "
        "         (o2 as string) = null and (o2 as Animal) = null then "
        "        good := good + 1; "
        "      end; "
        "      if q <> null and not (q! is string) and f is Object and "
        "         f(1) = 3 then "
        "        good := good + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(good); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));   // 3 * 100
}

// The second review's probes. A helper type for the rows below: one record
// with an `Object` field, one `Object` static, one iterator of `Object`.
namespace {

constexpr const char* kSiteDecls =
    "public record Rec(A: Object, B: int); "
    "public class Holder "
    "  public static readonly S: Object := Site.Twice; "
    "  public constructor() end constructor; "
    "end Holder; "
    "public class Dog "
    "  public constructor() end constructor; "
    "  public virtual method A(): int => 1; "
    "  public virtual method B(): int => 2; "
    "  public virtual method C(): int => 3; "
    "  public virtual method D(): int => 4; "
    "end Dog; "
    "public interface IBar "
    "  method Bar(): int; "
    "end IBar; "
    "public class Impl : IBar "
    "  public constructor() end constructor; "
    "  public method Bar(): int => 7; "
    "end Impl; "
    "public static class Site "
    "  public static method Twice(n: int): int => n * 2; "
    "  public static method T(o: Object?): string "
    "    if o = null then return \"null\"; end; "
    "    return $\"{o is string}/{o is Dog}/{o.ToString().Length > 0}\"; "
    "  end T; "
    "  public static method It(): IEnumerable<Object> "
    "    yield return Site.Twice; "
    "  end It; "
    "  public static method UseBar(b: IBar?): int "
    "    if b = null then return -1; end; "
    "    return b.Bar(); "
    "  end UseBar; "
    "end Site; ";

std::string siteMain(const std::string& body) {
    return std::string("module M; ") + kSiteDecls +
           "public class Program "
           "  public static method Main() " + body +
           "  end Main; "
           "end Program;";
}

}  // namespace

// §27.7.1 (v0.338): the hash "holds at `Object`, where a capture-free
// function value is admitted (§28.3's *Set-erasing positions*) and is an
// ordinary key: a `Set<Object>` holds two evaluations of one static method's
// reference as one element". One function value, reached as a function-typed local `f`, as
// the `Object` local `o` it was copied to, and as a fresh reference to the
// same method, is ONE key: every builtin collection member that takes a key
// or an element converts a function-typed argument exactly as an `Object`
// location does. Before this row's fix the argument reached the collection
// as the bare cell while the stored key was the object view (or the
// reverse), so `d.Add(f, 1); d[f]` threw `KeyNotFoundException`,
// `d.ContainsKey(o)` after `d.Add(f, 1)` answered `False`, and `d.Remove(f)`
// after `d.Add(o, 1)` answered `False`.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          function_and_object_spellings_are_one_collection_key_27_7_1) {
    const std::string src = siteMain(
        "    var f: (int) -> int := Site.Twice; "
        "    var o: Object := f; "
        "    var d := new Dictionary<Object, int>(); "
        "    d.Add(f, 1); "
        "    Console.WriteLine($\"{d[f]} {d[o]} {d.ContainsKey(o)} "
        "{d.ContainsKey(f)} {d.ContainsKey(Site.Twice)}\"); "
        "    var v: int? := null; "
        "    var hit := d.TryGetValue(f, out v); "
        "    Console.WriteLine($\"{hit} {v ?? -1}\"); "
        "    var d2 := new Dictionary<Object, int>(); "
        "    d2.Add(o, 2); "
        "    Console.WriteLine($\"{d2.ContainsKey(f)} "
        "{d2.ContainsKey(Site.Twice)} {d2[f]}\"); "
        "    Console.WriteLine($\"{d2.Remove(f)} {d2.Count}\"); "
        "    var d3 := new Dictionary<Object, int>(); "
        "    d3.Add(Site.Twice, 3); "
        "    Console.WriteLine($\"{d3.ContainsKey(o)} {d3.ContainsKey(f)}\"); "
        "    d3[f] := 9; "
        "    Console.WriteLine($\"{d3[o]} {d3.Count}\"); "
        "    var s := new Set<Object>(); "
        "    s.Add(f); "
        "    Console.WriteLine($\"{s.Contains(o)} {s.Contains(f)} {s.TryAdd(o)} "
        "{s.Count} {s.Remove(f)} {s.Count}\"); "
        "    var a := new Array<Object>(); "
        "    a.Add(f); "
        "    a.Add(\"x\"); "
        "    Console.WriteLine($\"{a.Contains(o)} {a.IndexOf(f)} "
        "{a.Contains(Site.Twice)} {Site.T(a[0])}\"); "
        "    Console.WriteLine($\"{o = f} {f = o} {o <> f}\"); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("1 1 True True True\n"
                                "True 1\n"
                                "True True 2\n"
                                "True 0\n"
                                "True True\n"
                                "9 1\n"
                                "True True False 1 True 0\n"
                                "True 0 True False/False/True\n"
                                "True True False\n"));
}

// §8.1 (v0.338): the positions the first fix left on the bare cell — each
// crashed in `bz_string_release` (ASan SEGV) or answered `is string` with
// `True`: an `Array<Object>` element store, a record `with` field, `??=` on
// `Object?`, the right operand of `??`, a `Dictionary<Object, V>` key read
// back through `Keys`, a dictionary VALUE stored by `Add` and by the
// indexer, `yield return` into an `IEnumerable<Object>`, and an `Object`
// static's initialiser.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          every_remaining_store_position_holds_an_ordinary_object_8_1) {
    const std::string src = siteMain(
        "    var f: (int) -> int := Site.Twice; "
        "    var a := new Array<Object>(); "
        "    a.Add(\"x\"); "
        "    a[0] := f; "
        "    Console.WriteLine(Site.T(a[0])); "
        "    var r := new Rec(\"s\", 1); "
        "    var r2 := r with(A := f); "
        "    Console.WriteLine(Site.T(r2.A)); "
        "    var p: Object? := null; "
        "    p ??= f; "
        "    Console.WriteLine(Site.T(p)); "
        "    var n: Object? := null; "
        "    var q: Object := n ?? f; "
        "    Console.WriteLine(Site.T(q)); "
        "    var d := new Dictionary<Object, int>(); "
        "    d.Add(f, 1); "
        "    for var k in d.Keys do Console.WriteLine(Site.T(k)); end; "
        "    var dv := new Dictionary<string, Object>(); "
        "    dv.Add(\"k\", f); "
        "    dv[\"j\"] := f; "
        "    Console.WriteLine($\"{Site.T(dv[\"k\"])} {Site.T(dv[\"j\"])}\"); "
        "    for var y in Site.It() do Console.WriteLine(Site.T(y)); end; "
        "    Console.WriteLine(Site.T(Holder.S)); ");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    const std::string one = "False/False/True\n";
    BEZEL_EXPECT_EQ(r.stdoutText,
                    one + one + one + one + one +
                        "False/False/True False/False/True\n" + one + one);
}

// §12.7 (v0.338): a function value at `Object` is no instance of any class
// and implements no interface. Its object view carries the six-word `Object`
// vtable prefix only — descriptor chain, interface map, `Dispose`,
// `ToString`, `Equals`, `GetHashCode` — so this row pins that a class test
// against a type with MORE virtual slots than that, an interface test, and
// the `as` forms all answer from the (null-terminated) descriptor chain and
// interface map and never read a slot the view does not have.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          function_value_is_no_class_and_no_interface_instance_12_7) {
    const std::string src = siteMain(
        "    var good: int := 0; "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      var o: Object := Site.Twice; "
        "      var dog := o as Dog; "
        "      var bar := o as IBar; "
        "      if not (o is Dog) and not (o is IBar) and not (o is Impl) and "
        "         dog = null and Site.UseBar(bar) = -1 then "
        "        good := good + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    using var impl := new Impl(); "
        "    var io: Object := impl; "
        "    Console.WriteLine($\"{good} {io is IBar} {Site.UseBar(io as IBar)}\"); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 True 7\n"));
}

// ---------------------------------------------------------------------------
// The third review's rows: the STORED and the QUERIED spelling of one function
// value must be the same `Object` in every builtin collection position.
//
// A conversion site is a (member, parameter) pair whose parameter is DECLARED
// at the receiver's own element / key / value type parameter (stdlib §4:
// `Add(item: T)`, `[key: K]`, `Raise(payload: T)`, `new Array<T>(size, fill:
// T)`, a collection literal's elements). The first cut selected sites by
// member NAME on any builtin generic receiver, which (a) converted an event
// HANDLER — `EventSource<Object>.Add(handler: (Object) -> ())` is a function
// used as one — so `Raise` executed a vtable as code, and (b) missed the
// stores that are not member calls (literals, the sized-fill constructor),
// so a stored bare cell never matched its converted query: `f in d`,
// `Contains`, `IndexOf`, `Remove` and the indexer all answered "absent".
//
// Each row runs every function-value kind (static reference, lambda, capturing lambda,
// second capturing lambda) for 100 iterations and asserts the
// printed answers (`Util.T` prints `is string` / `is Acc` of what it reads
// back: `False/False` for a function value, `True/False` for a string; that
// it is THE function type is #5826's subject). They run on `runAot`: a function value stored into a
// collection leaks its cell (#5967, see ORACLES above), which `runAotChecked`
// would report on every one of them.
namespace {

constexpr const char* kKindDecls =
    "module M; "
    "public class Acc "
    "  public _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method Add(x: int): int => x + _n; "
    "end Acc; "
    "public static class Util "
    "  public static method Twice(n: int): int => n * 2; "
    "  public static method Thrice(n: int): int => n * 3; "
    "  public static method T(o: Object?): string "
    "    if o = null then return \"null\"; end; "
    "    return $\"{o is string}/{o is Acc}\"; "
    "  end T; "
    "end Util; ";

struct FnKind { const char* name; const char* expr; };
constexpr FnKind kFnKinds[] = {
    {"static reference", "Util.Twice"},
    {"second capturing lambda", "lambda(x: int) => x + i + 1"},
    {"lambda", "lambda(x: int) => x + 1"},
    {"capturing lambda", "lambda(x: int) => x + i"},
};

// `One` binds `f` to the kind under test and `g` to a different function,
// runs `body` (which must define the string `s`) and returns `s`; `Main`
// calls it 100 times and prints the last answer.
std::string kindProgram(const FnKind& k, const std::string& members,
                        const std::string& body) {
    return std::string(kKindDecls) +
           "public class Program " + members +
           "  public static method One(i: int, a: Acc): string "
           "    var f: (int) -> int := " + k.expr + "; "
           "    var g: (int) -> int := Util.Thrice; " + body +
           "    return s; "
           "  end One; "
           "  public static method Main() "
           "    using var a := new Acc(5); "
           "    var i := 0; var r := \"\"; "
           "    while i < 100 do r := Program.One(i, a); i := i + 1; end; "
           "    Console.WriteLine(r); "
           "  end Main; "
           "end Program;";
}

void expectEveryKind(const std::string& members, const std::string& body,
                     const std::string& expected) {
    for (const FnKind& k : kFnKinds) {
        auto r = runAot(kindProgram(k, members, body));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, expected + "\n");
        if (r.stdoutText != expected + "\n")
            std::fprintf(stderr, "  ^ kind: %s\n", k.name);
    }
}

}  // namespace

// §27.7.1 "is an ordinary key" / §4.8 `key in dict`: a function key stored
// by `Add` and by the indexer is found by `in`, `ContainsKey`, the indexer
// and `Remove`. (`f in d` answered False; the
// capturing kinds overflowed the stack in the key hash.)
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          dictionary_object_key_is_found_by_in_and_indexer_27_7_1) {
    expectEveryKind("",
        "    var d := new Dictionary<Object, int>(); "
        "    d.Add(f, 1); "
        "    d[g] := 2; "
        "    var s := $\"{f in d} {g in d} {d.ContainsKey(f)} {d[f]} {d[g]} "
        "{d.Remove(f)} {f in d} {d.Count}\"; ",
        "True True True 1 2 True False 1");
}

// Stdlib §4.4 `new Array<T>(size, fill: T)`: the fill value is a store.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          sized_fill_array_of_object_holds_the_object_view_8_1) {
    expectEveryKind("",
        "    var arr := new Array<Object>(3, f); "
        "    var s := $\"{arr.Count} {arr.Contains(f)} {arr.IndexOf(f)} "
        "{arr.Contains(g)} {f in arr} {Util.T(arr[2])} {arr.Remove(f)} "
        "{arr.Count}\"; ",
        "3 True 0 False True False/False True 2");
}

// §15.4: a dictionary literal's keys and values are stores at K and V.
// (`d[f]` raised KeyNotFoundException.)
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          dictionary_literal_keys_and_values_at_object_15_4) {
    expectEveryKind("",
        "    var d: Dictionary<Object, int> := [f: 1, g: 2]; "
        "    var d2: Dictionary<int, Object> := [1: f, 2: g]; "
        "    var s := $\"{d.Count} {d[f]} {d[g]} {d.ContainsKey(g)} "
        "{Util.T(d2[1])} {d2[1] = d2[2]}\"; ",
        "2 1 2 True False/False False");
}

// §15.4: a set literal's elements. (`Contains` answered False, and the
// duplicate `f` was not folded for a non-capturing kind's sibling spelling.)
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          set_literal_elements_at_object_15_4) {
    expectEveryKind("",
        "    var st: Set<Object> := [f, g, f]; "
        "    var s := $\"{st.Count} {st.Contains(f)} {st.Contains(g)} "
        "{f in st} {st.Remove(f)} {st.Contains(f)}\"; ",
        "2 True True True True False");
}

// §15.4: the linked-list, queue and stack literal forms, each read back.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          linked_list_queue_and_stack_literals_at_object_15_4) {
    expectEveryKind("",
        "    var ll: LinkedList<Object> := [f, g]; "
        "    var q: Queue<Object> := [f, g]; "
        "    var sk: Stack<Object> := [f, g]; "
        "    var s := $\"{ll.Contains(f)} {ll.Remove(g)} {ll.Contains(g)} "
        "{ll.Count} {q.Contains(f)} {q.Contains(g)} {sk.Contains(g)} "
        "{Util.T(q.Dequeue())} {Util.T(sk.Pop())}\"; ",
        "True True False 1 True True True False/False False/False");
}

// Stdlib §4.15 `ArraySpan<T>.Fill(value: T)` / `IndexOf` / `Contains`: a
// store and two queries through a view of an `Array<Object>`.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          array_span_fill_and_queries_at_object_8_1) {
    expectEveryKind("",
        "    var arr := new Array<Object>(); "
        "    arr.Add(g); arr.Add(\"x\"); arr.Add(g); "
        "    var sp := arr.AsSpan(); "
        "    sp.Fill(f); "
        "    var s := $\"{arr.IndexOf(f)} {arr.Contains(g)} {sp.Contains(f)} "
        "{sp.IndexOf(f)} {Util.T(arr[2])}\"; ",
        "0 False True 0 False/False");
}

// Stdlib §6.5 `EventSource<T>`: `Add(handler)` takes a FUNCTION — it is
// never converted, whatever `T` is — while `Raise(payload: T)` stores the
// payload at `T`. The handler sees two function payloads and one string
// (1 + 10 + 1); after `Remove` it sees nothing. (Converting the handler made
// `Raise` jump into the function object's vtable: SEGV.)
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          event_source_of_object_handler_is_a_function_payload_an_object_8_1) {
    expectEveryKind("",
        "    using var src := new EventSource<Object>(); "
        "    var n := 0; "
        "    var h: (Object) -> () := lambda(o: Object) => do "
        "      if o is string then n := n + 10; else n := n + 1; end; "
        "    end; "
        "    src.Add(h); "
        "    src.Raise(f); "
        "    src.Raise(\"x\"); "
        "    src.Raise(g); "
        "    src.Remove(h); "
        "    src.Raise(f); "
        "    var s := $\"{n}\"; ",
        "12");
}

// The same for `AsyncEventSource<Object>` with a method-reference handler
// (stdlib §6.5: `Add(handler)`, `RaiseAsync(payload: T)`).
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          async_event_source_of_object_handler_is_a_function_8_1) {
    const std::string src = std::string(kKindDecls) +
        "public class Program "
        "  public static async method On(o: Object): Task "
        "    await Task.Delay(1); "
        "    Console.WriteLine($\"{o is string} {o is Acc}\"); "
        "  end On; "
        "  public static async method Main(): Task<int> "
        "    using var src := new AsyncEventSource<Object>(); "
        "    src.Add(Program.On); "
        "    using var t := src.RaiseAsync(\"x\"); "
        "    await t; "
        "    using var u := src.RaiseAsync(Util.Twice); "
        "    await u; "
        "    src.Remove(Program.On); "
        "    using var v := src.RaiseAsync(\"y\"); "
        "    await v; "
        "    Console.WriteLine(\"done\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True False\nFalse False\ndone\n"));
}

// §7.7 (v0.338): "for a left operand of type `T?`, the right operand must be
// implicitly convertible (§5.1) to `T` or to `T?`". With `T` = `Object` the
// operands may be a function value and a string, and each is converted on
// ITS OWN type — the function, never the string — whichever side it is on.
// In declaration, argument, assignment and return position (expression-
// bodied and block-bodied). (The merged value was converted: SEGV in
// `bz_string_retain`.)
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          null_coalesce_at_object_converts_each_operand_on_its_own_type_7_7) {
    expectEveryKind(
        "  public static method Pick(some: bool): Object? "
        "    if some then return Util.Twice; end; "
        "    return null; "
        "  end Pick; "
        "  public static method Co(some: bool): Object => "
        "    Program.Pick(some) ?? \"r\"; "
        "  public static method Cb(some: bool): Object "
        "    return Program.Pick(some) ?? \"b\"; "
        "  end Cb; ",
        "    var nn: Object? := null; "
        "    var some: Object? := f; "
        "    var o: Object := nn ?? \"s\"; "
        "    var p: Object := some ?? \"s\"; "
        "    var q: Object := nn ?? f; "
        "    var arr := new Array<Object>(); "
        "    arr.Add(nn ?? \"t\"); "
        "    arr.Add(some ?? \"t\"); "
        "    arr.Add(nn ?? g); "
        "    o := nn ?? \"v\"; "
        "    var s := $\"{Util.T(o)} {Util.T(p)} {Util.T(q)} {Util.T(arr[0])} "
        "{Util.T(arr[1])} {Util.T(arr[2])} {arr.Contains(f)} "
        "{arr.Contains(g)} {Util.T(Program.Co(false))} "
        "{Util.T(Program.Co(true))} {Util.T(Program.Cb(false))} "
        "{Util.T(Program.Cb(true))}\"; ",
        "True/False False/False False/False True/False False/False "
        "False/False True True True/False False/False True/False "
        "False/False");
}

// The same pair with no collection and no function local in play, looped
// under the generated-code sanitizer: the string operand is retained and
// released as a string, in declaration, assignment and return position.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          null_coalesce_at_object_string_arm_is_sanitizer_clean_7_7) {
    const std::string src = std::string(kKindDecls) +
        "public class Program "
        "  public static method Co(nn: Object?): Object => "
        "    nn ?? \"r\"; "
        "  public static method Cb(nn: Object?): Object "
        "    return nn ?? \"b\"; "
        "  end Cb; "
        "  public static method Main() "
        "    var good := 0; var i := 0; "
        "    while i < 100 do "
        "      var nn: Object? := null; "
        "      var o: Object := nn ?? \"s\"; "
        "      var p := Program.Co(nn); "
        "      var q := Program.Cb(nn); "
        "      if o is string and p is string and q is string then "
        "        good := good + 1; "
        "      end; "
        "      o := nn ?? \"v\"; "
        "      if o.ToString() = \"v\" then good := good + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(good); "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);                  // ASan/LSan-clean run, not a skip
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// `nn ?? throw …` (§17.12) and `nn ?? null` over a nullable function `nn`.
// §7.7 types the first at the function type and the second at the nullable
// function type: the value is a function-typed reference (the cell base),
// and it is converted only if the position it then reaches is `Object`.
namespace {

constexpr const char* kCoalDecls =
    "module M; "
    "import System; "
    "public record RB(F: (int) -> int); "
    "public class Slot "
    "  public G: ((int) -> int)?; "
    "  public constructor() G := null; end constructor; "
    "end Slot; "
    "public static class Util "
    "  public static method Twice(n: int): int => n * 2; "
    "  public static method Thrice(n: int): int => n * 3; "
    "  public static method Ap(f: (int) -> int, n: int): int => f(n); "
    "  public static method ApN(f: ((int) -> int)?, n: int): int "
    "    if f = null then return -1; end; "
    "    return f(n); "
    "  end ApN; "
    "  public static method Id<T>(x: T): T => x; "
    "  public static method T(o: Object?): string "
    "    if o = null then return \"null\"; end; "
    "    return $\"{o is string}\"; "
    "  end T; "
    "end Util; ";

struct CoalSrc { const char* name; const char* pick; };
// A static method reference and a capturing lambda, each as a call result.
constexpr CoalSrc kCoalSrcs[] = {
    {"static reference",
     "    if some then return Util.Twice; end; "},
    {"capturing lambda",
     "    if some then return lambda(x: int) => x * 2 + k - k; end; "},
};

// `Main` runs `body` 100 times; `body` adds to `t`, which is printed. `nn`
// is the non-null nullable function, `no` the null one.
std::string coalProgram(const CoalSrc& k, const std::string& members,
                        const std::string& body) {
    return std::string(kCoalDecls) +
           "public class Program "
           "  public static method Pick(some: bool, k: int): ((int) -> int)? " +
           k.pick +
           "    return null; "
           "  end Pick; " + members +
           "  public static method Main() "
           "    var t := 0; var i := 0; "
           "    while i < 100 do "
           "      var nn := Program.Pick(true, i); "
           "      var no := Program.Pick(false, i); " + body +
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(t); "
           "  end Main; "
           "end Program;";
}

void expectCoalClean(const std::string& members, const std::string& body,
                     const std::string& expected) {
    for (const CoalSrc& k : kCoalSrcs) {
        auto r = runAotChecked(coalProgram(k, members, body));
        BEZEL_EXPECT_TRUE(r.ran);              // ASan/LSan-clean run, not a skip
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, expected + "\n");
        if (!r.ran || r.stdoutText != expected + "\n")
            std::fprintf(stderr, "  ^ source: %s\n", k.name);
    }
}

}  // namespace

// At a FUNCTION-typed declaration, assignment and direct call, and through a
// right-associative chain: no `Object` position is involved, so nothing is
// converted and the value is invoked as the cell it is. These programs ran
// before #5978 and must keep running; a conversion keyed on "exactly one
// operand is function-valued" handed `g` the object view and the call ran the
// vtable as code (SEGV in `bz_fnval_vtable`).
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          coalesce_throw_and_null_at_function_typed_locals_stay_cells_7_7) {
    expectCoalClean(
        "",
        "      var g: (int) -> int := "
        "        nn ?? throw new InvalidOperationException(\"x\"); "
        "      var h: ((int) -> int)? := nn ?? null; "
        "      var z: ((int) -> int)? := no ?? null; "
        "      var a: (int) -> int := Util.Thrice; "
        "      a := nn ?? throw new InvalidOperationException(\"x\"); "
        "      var b: ((int) -> int)? := null; "
        "      b := nn ?? null; "
        "      var c: (int) -> int := "
        "        no ?? nn ?? throw new InvalidOperationException(\"x\"); "
        "      var d: ((int) -> int)? := no ?? nn ?? null; "
        "      t := t + g(1) + Util.ApN(h, 1) + Util.ApN(z, 1) + a(1) "
        "             + Util.ApN(b, 1) + c(1) + Util.ApN(d, 1) "
        "             + (nn ?? throw new InvalidOperationException(\"x\"))(1); ",
        "1300");                               // (2*7 - 1) * 100
}

// The same at a function-typed ARGUMENT, RETURN (block- and expression-
// bodied), record component, class field and generic type argument.
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          coalesce_throw_and_null_at_function_typed_positions_stay_cells_7_7) {
    expectCoalClean(
        "  public static method R(i: int): (int) -> int "
        "    var nn := Program.Pick(true, i); "
        "    return nn ?? throw new InvalidOperationException(\"x\"); "
        "  end R; "
        "  public static method Rx(i: int): (int) -> int => "
        "    Program.Pick(true, i) ?? throw new InvalidOperationException(\"x\"); ",
        "      var r1 := Program.R(i); "
        "      var r2 := Program.Rx(i); "
        "      var rb := new RB(nn ?? throw new InvalidOperationException(\"x\")); "
        "      var sl := new Slot(); "
        "      sl.G := nn ?? null; "
        "      var g1 := Util.Id<(int) -> int>( "
        "        nn ?? throw new InvalidOperationException(\"x\")); "
        "      var g2 := Util.Id<((int) -> int)?>(nn ?? null); "
        "      t := t + Util.Ap(nn ?? throw new InvalidOperationException(\"x\"), 1) "
        "             + Util.ApN(nn ?? null, 1) + Util.ApN(no ?? null, 1) "
        "             + r1(1) + r2(1) + rb.F(1) + Util.ApN(sl.G, 1) "
        "             + g1(1) + Util.ApN(g2, 1); ",
        "1500");                               // (2*8 - 1) * 100
}

// At an `Object` TARGET the whole `??` is a function value (or null) and
// takes the object view once, at the position: declaration, assignment and
// argument, with a `throw`, a `null` and a function fallback. (`Util.T`
// answered `True` — "is a string" — for each before #5978.)
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          coalesce_throw_and_null_at_object_target_take_the_view_once_7_7) {
    expectCoalClean(
        "",
        "      var o: Object := "
        "        nn ?? throw new InvalidOperationException(\"x\"); "
        "      var p: Object? := nn ?? null; "
        "      var z: Object? := no ?? null; "
        "      var q: Object := nn ?? Util.Thrice; "
        "      var a: Object := \"s\"; "
        "      a := nn ?? throw new InvalidOperationException(\"x\"); "
        "      var s := $\"{Util.T(o)} {Util.T(p)} {Util.T(z)} {Util.T(q)} "
        "{Util.T(a)} "
        "{Util.T(nn ?? throw new InvalidOperationException(\"x\"))} "
        "{Util.T(nn ?? null)} {Util.T(no ?? null)}\"; "
        "      if s = \"False False null False False False False null\" then "
        "        t := t + 1; "
        "      end; ",
        "100");
}

// §22.5 a function-valued DEFAULT for an `Object` parameter, and §30.5 an
// async method's `return` into its `Task<Object>`: both are stores at
// `Object` that no declared-type conversion sees. (Both held the bare cell:
// `Util.T` released it as a string.)
BEZEL_TEST(function_value_at_object_spec_12_7_5978,
          default_parameter_and_async_return_at_object_22_5) {
    expectEveryKind(
        "  public static method Dflt(o: Object := Util.Twice): string => "
        "    Util.T(o); "
        "  public static async method Ta(): Task<Object> "
        "    return Util.Twice; "
        "  end Ta; ",
        "    using var t := Program.Ta(); "
        "    var o: Object := f; "
        "    var s := $\"{Program.Dflt()} {Program.Dflt(o)} "
        "{Util.T(t.Result)}\"; ",
        "False/False False/False False/False");
}
