//! case: s28_14_9311
//! doc: spec
//! section: 28.14
//! title: two call-bounded function-typed parameters, one feeding the other, are both invoked in the callee
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.14, 28.3
//! expect: run
//! output: 4000
//! notes: Harvested from the #9174 review (m_two_formals). `Get(a, b)` computes `b(a())` and returns a closure over it. Per iteration x = 5: a() = 5, b(5) = 5 + 5 = 10; two calls of the closure add 20; 200 iterations: 4000.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Get(a: () -> int, b: (int) -> int): () -> int
    var v := b(a());
    return lambda() => v;
  end Get;
end Fl;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      using var fl := new Fl();
      var x := 5;
      var f := fl.Get(lambda() => x, lambda(y: int) => y + x);
      t := t + f() + f();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
