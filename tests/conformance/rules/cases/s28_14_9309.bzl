//! case: s28_14_9309
//! doc: spec
//! section: 28.14
//! title: a nullable function-typed parameter invoked after a null test, its value captured by a closure
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.14, 28.3
//! expect: run
//! output: 1000
//! notes: Harvested from the #9174 review (k_nullable). `Get` tests `q` for null and invokes it only if present. Per iteration x = 5: the first call has v = 5 and its closure yields 5; `Get(null)` leaves v = 0, yielding 0; 5 per iteration, 200 iterations: 1000.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Get(q: (() -> int)?): () -> int
    var v := 0;
    if q <> null then v := q(); end if;
    return lambda() => v;
  end Get;
end Fl;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      using var fl := new Fl();
      var x := 5;
      var f := fl.Get(lambda() => x);
      t := t + f();
      var g := fl.Get(null);
      t := t + g();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
