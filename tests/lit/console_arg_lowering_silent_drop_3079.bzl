// fix #3079 (stdlib §2.1 / §21.9 / §3 permissive acceptance, v0.249):
// diagnostic pin for THE THIRD VALUE-DEMANDING DEGRADE — the one #2966 named as
// a residual, and the only one of the three whose degrade is not a wrong value
// but the DISAPPEARANCE OF THE WHOLE STATEMENT.
//
// `tryLowerConsoleCall` (LlvmBackend_Calls.cpp) positively identifies
// `Console[.Error|.Out].Write(Line)` with exactly one argument and then, if the
// argument fails to lower, returned `false` — "not a console call". Nothing
// downstream matches that shape, so lowerCallDispatch ran out of arms and its
// `if (!ms)` chain returned nullptr, and a nullptr call result in statement
// position emits NOTHING: not the value, not the line terminator, no diagnostic
// at any severity, exit 0. Measured that way on 309356e for a class-level
// `const` receiver and for an inline array's `.Length` (both now typed in sema
// by the same fix), and recorded verbatim twice before at
// LlvmBackend_Calls2.cpp's #2982 and #2469 notes for `?.` receivers.
//
// stdlib §2.1, verbatim: `public static method WriteLine(value: Object);` —
// there is no argument-less-value overload; "The `Object` overloads invoke
// `ToString()`". §21.9, verbatim: "A method call, an `await` expression, or any
// other expression whose result is `()` may appear as a statement on its own
// line, terminated with `;`" — the argument position is not such a place.
//
// ACCEPT-WITH-WARNING, deliberately, on THIS program: the argument is an
// expression sema ADMITS and codegen cannot lower, so the §3/#3079 degrade is
// legitimately reached, and an Error here would be exactly the false reject
// #2434's lit-corpus check found for the local-decl twin. The severity is
// #2434/#2491's, reused: a Warning, printed by
// `Driver::compileExecutableObject` like every diagnostic, so the drop is
// visible on stderr while the build still succeeds. The companion AOT test
// `console_tostring_untyped_receiver_3079_test.cpp` pins the surrounding lines
// still printing and the exit code still 0.
//
// ⚠ THE TRIGGER WAS RE-BASED BY fix #7973 (§8.1, v0.350). This case used to
// use `o.Anything()` on an `Object`-typed receiver — the §3/#605 permissive
// "unknown member" acceptance. §8.1 declares the root `Object`'s public
// surface ("The declaration above defines the public surface"), and §26.5
// resolves a member against the receiver's declared type, so that shape is
// now a COMPILE ERROR and never reaches codegen. The subject of this case —
// a Console argument that fails to lower, and the whole statement disappearing
// with it — is unchanged; only the shape that reaches it moved, to the one
// legal shape that still does: an indexer read on a BARE TYPE PARAMETER, whose
// member set §3.6 rule 2 / §14.2 leave open, so sema admits the read while
// the monomorphized clone at `T = int` has no indexer to dispatch to.
//
// The BUILT-IN-receiver subpopulation escalates to a hard error instead; that
// is `console_arg_builtin_unit_call_rejected_3079.bzl`.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module N3079Lit;
class C
  public static method Main(): int
    Console.WriteLine("<");
    _ := C.Probe(1);
    Console.WriteLine(">");
    return 0;
  end Main;
  public static method Probe<T>(x: T): int
    Console.WriteLine(x[0]);
    Console.Write(x[0]);
    return 0;
  end Probe;
end C;

// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: the argument of this `Console.WriteLine` failed to lower to a value, so the whole statement emits nothing — not even the line terminator (stdlib §2.1 / §21.9) — #3079
// CHECK-DAG: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} internal: the argument of this `Console.Write` failed to lower to a value, so the whole statement emits nothing — not even the line terminator (stdlib §2.1 / §21.9) — #3079
