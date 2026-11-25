//! case: s28_14_9310
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback forwarded through static helpers: invoked, discarded and returned onward
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.14, 28.3
//! expect: run
//! output: 4800
//! notes: Harvested from the #9174 review (l_static_chain). Per iteration x = 5: `Fl.Get(lambda() => x)()` adds 5; the discarded `Fl.Get(lambda() => x);` adds nothing; `Fwd` forwards `q` to `Get` (a call-bounded parameter), so `f()` is 5 and two calls add 10; `Fwd2` builds its own closure over its local 9, so `g()` adds 9: 24 per iteration, 200 iterations: 4800.
module Conf;
public class Fl
  public static method Get(q: () -> int): () -> int
    var v := q();
    return lambda() => v;
  end Get;
  public static method Fwd(q: () -> int): () -> int
    return Fl.Get(q);
  end Fwd;
  public static method Fwd2(): () -> int
    var x := 9;
    return Fl.Get(lambda() => x);
  end Fwd2;
end Fl;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var x := 5;
      t := t + Fl.Get(lambda() => x)();
      Fl.Get(lambda() => x);
      var f := Fl.Fwd(lambda() => x);
      t := t + f() + f();
      var g := Fl.Fwd2();
      t := t + g();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
