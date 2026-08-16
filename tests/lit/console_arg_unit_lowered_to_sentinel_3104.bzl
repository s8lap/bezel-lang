// fix #3104 (spec §4.7 / §8.5 / §21.9 / stdlib §2.1, v0.255): the §3079
// guard's branch (a) — typed-unit exemption — silently dropped the whole
// `Console.WriteLine(<unit>)` statement, while branch (b) — untyped built-
// in receiver — HARD-ERRORED on the same value-less shape. The two
// branches disagreed on what the spec says for `Console.WriteLine(())`:
// §4.7 admits the boxing, §8.5 gives the rendering, §21.9 does not
// prohibit argument position, stdlib §2.1 declares the `Object` overload.
//
// The fix is `tryLowerConsoleCall` (LlvmBackend_Calls.cpp) restoring the
// §8.5 `"()"` sentinel when the argument type is unit, then proceeding
// past the guard with `arg` set to that sentinel. The accompanying gtest
// `console_write_unit_arg_renders_3104_test.cpp` pins the rendered
// output (covering the literal `()`, a unit method-call, `Console.Write`
// without `Line`, AND the sibling-3079 permissive shape that still
// warns); THIS lit test pins the diagnostic surfacing — the §3079
// guard MUST NOT emit a `internal: … emits nothing … #3079` Warning
// for a unit-typed argument, because the statement no longer emits
// nothing.
//
// The sibling branch (b) case (`Console.WriteLine(o.Anything())` — a
// built-in-receiver undefined-member call) is intentionally NOT
// included in this lit test: branch (b) STILL emits the §3079 warning
// by spec, and FileCheck-NOT cannot simultaneously express "this case
// silences, that case warns" — that dual shape is pinned by the gtest
// above, where each case is a discrete assertion.

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --allow-empty %s

module N3104Lit;
class C
  public static method ReturnsUnit(): ()
    return;
  end ReturnsUnit;
  public static method Main(): int
    Console.WriteLine("a");
    Console.WriteLine(ReturnsUnit());
    return 0;
  end Main;
end C;

// NEGATIVE — the unit-typed argument MUST NOT earn the §3079 "emits
// nothing" Warning; the fix restores the §8.5 sentinel instead. A
// regression to the silent-drop (or one that re-exempts the warning
// path) emits exactly the matched line and FileCheck fails.
// CHECK-NOT: Console.WriteLine` failed to lower to a value