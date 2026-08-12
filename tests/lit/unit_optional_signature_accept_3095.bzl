// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// The DIAGNOSTIC-SILENCE oracle for fix #3095 (language spec §4.7 "The unit
// type", v0.252; §21.7).
//
// §4.7: "Wrapping the unit type with `?` (yielding `()?`) is permitted by the
// grammar and produces an optional with two values, `null` and `()`."
//
// The gtest sibling (tests/codegen/unit_optional_signature_3095_test.cpp) owns
// the VALUES; this file owns the silence, because `runAot` records diagnostics
// only on a compile REJECT — a Warning on a program that compiles is invisible
// to it, so "no `internal:` residual" is a property only the lit track can
// actually assert.
//
// What the residuals were, measured on 663a68e:
//   * a `()?` (or plain `()`) PARAMETER did not warn — it TRAPPED the compiler
//     (SIGILL, exit 132, no diagnostic at all) in bindParameters' alloca;
//   * `u := ();` on a `()?` local emitted `internal: assignment RHS expression
//     failed to lower to a value … (store dropped — target retains its previous
//     value) — #2961` and left `u` reading as `null`;
//   * `new R(1, ())` at a `U: ()?` record field emitted `internal:
//     lowerLocalDecl's initializer expression failed to lower to a value …
//     — #2434` and degraded the WHOLE record;
//   * a `()?` RETURN type was rejected outright by §21.7.
// Each is now a real lowering, so the whole file must compile in silence.
//
// NODIAG-NOT: internal:
// NODIAG-NOT: this callable has no return type
// NODIAG-NOT: #2961
// NODIAG-NOT: #2434

module ACC3095;

record RU(A: int, U: ()?);

class ACC
  // (a) `()?` at a PARAMETER — the SIGILL arm.
  public static method Take(u: ()?): int
    if u = null then return 1; end;
    return 2;
  end Take;

  // (a') the NON-nullable unit at a parameter — the same trap.
  public static method TakePlain(u: ()): int
    return 7;
  end TakePlain;

  // (b) `()?` as a RETURN type — the false-reject arm, both values.
  public static method Nul(): ()?
    return null;
  end Nul;

  public static method Some(): ()?
    return ();
  end Some;

  // The accept-control: a plain `()` return type is still a void-method
  // context (§4.7: `()` "is the return type of methods declared without an
  // explicit return type"), so a bare `return;` is its correct form.
  public static method Nothing(): ()
    return;
  end Nothing;

  public static method Main(): int
    var total: int := 0;

    // Locals, both values, and ASSIGNMENT in both directions.
    var u: ()? := null;
    var v: ()? := ();
    if u = null then total := total + 1; end;
    if v <> null then total := total + 2; end;
    u := ();
    if u <> null then total := total + 4; end;
    u := null;
    if u = null then total := total + 8; end;

    // Arguments: through a local and written directly at the call site.
    total := total + ACC.Take(u) + ACC.Take(v) +
             ACC.Take(null) + ACC.Take(()) + ACC.TakePlain(());

    // Returns, and the round trip back into a `()?` local.
    if ACC.Nul() = null then total := total + 16; end;
    if ACC.Some() <> null then total := total + 32; end;
    var w: ()? := ACC.Some();
    if w <> null then total := total + 64; end;

    // A `()?` RECORD FIELD, both values.
    var ra := new RU(1, null);
    var rb := new RU(2, ());
    if ra.U = null then total := total + 128; end;
    if rb.U <> null then total := total + 256; end;

    // A LAMBDA parameter, both forms (§27.1). `lowerLambda` binds each
    // parameter by storing the incoming argument into its slot, and a `void`
    // parameter type made that store trap too
    // (`DataLayout::getABITypeAlign(void)`, SIGILL) — a THIRD site with the
    // same defect, distinct from bindParameters'. The `()`-parameter closure is
    // declared and not called on purpose: the function-VALUE call path has its
    // own argument builder that does not yet supply the unit operand (filed
    // separately), so calling it would emit a #3079 note this file forbids.
    var lu: (()) -> int := lambda (u: ()) => 6;
    var lo: (()?) -> int := lambda (u: ()?) => do
      if u = null then return 1; end;
      return 2;
    end;
    total := total + lo(null) + lo(w);

    // The non-nullable unit at its own already-working positions (#1619 local,
    // #2915 box) — untouched by the `()?` representation.
    var un: () := ();
    var o: Object := ();
    ACC.Nothing();
    Console.WriteLine(o.ToString());
    Console.WriteLine(total);
    return 0;
  end Main;
end ACC;
