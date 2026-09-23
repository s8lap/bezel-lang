// fix #4665 (codegen; language spec §17.11 *The `default` operator* — the UNIT
// row — with §4.7 *The unit type*, §14.2 *Constraints* and §21.7 *The return
// statement*, v0.307).
//
// THIS FILE IS THE DIAGNOSTIC ORACLE for the second half of #4665. The first
// half — that `default(T)` at `()` no longer kills `bezelc` with SIGILL, and
// that every spelling of it produces the right value at run time — is pinned in
// tests/codegen/default_unit_type_4665_test.cpp. It has to be split that way:
// the AOT gtest harness populates `JitResult::diagnostics` only on a compile
// REJECT, so a successful compile's WARNINGS never reach it and an "assert no
// note" written there would pass vacuously on a fixed and an unfixed compiler
// alike (the finding recorded in
// tests/codegen/call_lowered_as_unit_vs_declined_2963_test.cpp).
//
// WHAT IT PINS. Once the `DefaultExpr` arm answers `nullptr` for the unit type
// — the backend's one representation of "this expression produced no lowered
// value" (#3421) — `return default(T);` inside a generic body specialized at
// `()` reaches the §21.7 return site with no value, which is CORRECT: the
// specialized function's LLVM return type is `void` and it emits `ret void`.
// The #2491 backstop's unit exemption tested `ctx_.exprType(...)->kind ==
// Kind::Unit`, and a bare type parameter arrives there as a `Named T` with no
// declaration, so the exemption missed and a working program printed
//
//   internal: return-value expression failed to lower to a value
//             (degraded to a zero/null return value) — #2491
//
// Nothing degraded, so nothing may say it did: #2434/#2491/#2775 exist to create
// a signal for SILENT codegen degrades, and a copy of that signal on a correct
// program erodes the only evidence the next reader has. The exemption now
// resolves through the active monomorphization first (`resolveTypeParamRef`,
// the predicate #3137's unit-argument rescue already uses for this exact
// shape).
//
// FOUR ROUTES to a unit `default`, so this cannot pass by one being
// special-cased: the specialized generic's bare-`T` return (the issue's repro),
// the directly-written `default(())` §17.11 row, §22.5's omitted-argument
// parameter default, and the §22.2 ARROW body `=> default(())`.
//
// The arrow body is a SECOND #2491 face with its own predicate. #3434 already
// made a `: ()` ascription answer "yields nothing", but only along the CallExpr
// path inside `arrowBodyLoweredAsUnit`, whose call-shape gate short-circuits
// before the ascription is consulted — so a non-call unit body such as
// `default(())` still drew the note. Measured with the `DefaultExpr` arm fixed
// and this one not: `Give(): () => default(());` compiled, ran, and printed
// `y2.bzl:3:38: internal: return-value expression failed to lower to a value
// (degraded to a zero/null return value) - #2491`. Its two negative controls
// (`: int` and `: ()?`) live beside this file and must keep their notes.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// internal: / #2491 diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class Factory<T>
  public method Make(): T return default(T); end Make;   // §14.2 at `()`
end Factory;

class C
  public static method Take(p: () := default(())): string   // §22.5
    return p.ToString();
  end Take;
  public static method Give(): () => default(());           // §22.2 arrow body
  public static method Main(): int
    using var fu := new Factory<()>();
    var u := fu.Make();
    var d: () := default(());                              // §17.11 unit row
    Console.WriteLine(u.ToString());
    Console.WriteLine(d.ToString());
    Console.WriteLine(C.Take());
    C.Give();
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: #2491
