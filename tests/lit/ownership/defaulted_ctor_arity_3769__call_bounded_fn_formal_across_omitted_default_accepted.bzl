// RUN: %bezelc -c -o %t.o %s
//
// Issue #3769, ANTI-OVER-REJECT. Resolving formals for a defaulted-arity call
// arms consumers that could not see this shape before, and §28.14's
// call-bounded gate (`verifiedCallBoundedFormalAt`) is one of them: a plain
// function-typed constructor parameter is call-bounded, and until this fix that
// rule simply did not run for a call omitting a §22.5 default. INVOKING such a
// parameter is exactly what §28.14 admits — a call-bounded parameter may be
// "invoked, bound to locals, and passed on as an argument to another
// call-bounded parameter" — so this program must still compile clean.

module M;

class Holder
  private _v: int;
  public constructor(cb: () -> int, n: int := 3)
    _v := cb() + n;
  end constructor;
  public method Value(): int => _v;
end Holder;

class C
  public static method Main(): int
    var h := new Holder(lambda() => 4);
    return h.Value();
  end Main;
end C;
