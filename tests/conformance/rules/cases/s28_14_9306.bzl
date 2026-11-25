//! case: s28_14_9306
//! doc: spec
//! section: 28.14
//! title: a callback passed down to a helper inside a wrapping lambda is released without affecting the result
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.14, 28.3
//! expect: run
//! output: 600
//! notes: Harvested from the #9174 review (c_inner_wrap). `Get` hands `lambda() => q()` to the call-bounded parameter `h` of `Helper`, which ignores it and returns `lambda() => 3`. Per iteration f() = 3; 200 iterations: 600. The wrapper lambda captures `q` but is only passed to a call-bounded parameter, which the rule allows.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Helper(h: () -> int): () -> int
    return lambda() => 3;
  end Helper;
  public method Get(q: () -> int): () -> int
    var r := this.Helper(lambda() => q());
    return r;
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
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
