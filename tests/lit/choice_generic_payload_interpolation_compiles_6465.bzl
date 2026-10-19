// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// NODIAG-NOT: error:

// fix #6465 (codegen, the struct-typed interpolation-hole dispatch in
// LlvmBackend_Exprs.cpp, v0.338): that branch classified the hole's choice at
// the instantiation (`structRt`) and then rendered it WITHOUT one —
// `emitChoiceToString(ev, ts)` left `instTy` at its null default. With no
// instantiation the renderer took its resolveTypeParamRef else-arm, which
// returns its argument unchanged when the active substitution has no entry for
// the head; at a hole that substitution is empty, so a payload spelled `T`
// resolved to the type parameter and emitValueToString rendered the empty
// string. MEASURED: `$"{W<Col>.Ok(pn)}"` printed `W.Ok(v: )` while the
// identical value's `.ToString()` printed `W.Ok(v: null)`.
//
// §35.7 fixes the rendering as "TypeName.CaseName(field1: value1, ...)" and
// §18.5's rule that an absent value renders as the four characters `null`
// ("Rendering `null` rather than the empty string keeps absent values
// diagnosable in output"), so the empty component is a wrong-output defect.
//
// The FIRST RUN line is the load-bearing one: it fails if the substituted
// payload cannot be dispatched. The second pins that the compiler is SILENT on
// this program — supplying the instantiation must not have bought a
// diagnostic, which would trade a wrong rendering for a false rejection.
//
// This is the FOURTH spelling of the #6169 / #6458 payload-substitution class
// and the first with a different mechanism: the other three substituted and
// then dropped the declared `?`; this one never substituted at all.
//
// per §8.5, §18.5, §35.7, v0.338
module M;
choice Col Red, Rgb(v: int); end Col;
choice W<T> Ok(v: T?), No; end W;
choice WN<T> Ok(v: T), No; end WN;
class Program
  public static method Main(): int
    var pn: Col? := null;
    var pv: Col? := Col.Rgb(7);
    var c := Col.Rgb(7);
    var n := W<Col>.Ok(pn);
    var v := W<Col>.Ok(pv);
    var b := WN<Col>.Ok(c);
    Console.WriteLine($"{n}");
    Console.WriteLine(n.ToString());
    Console.WriteLine($"{v}");
    Console.WriteLine(v.ToString());
    Console.WriteLine($"{b}");
    Console.WriteLine(b.ToString());
    return 1;
  end Main;
end Program;