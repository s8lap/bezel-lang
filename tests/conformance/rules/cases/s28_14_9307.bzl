//! case: s28_14_9307
//! doc: spec
//! section: 28.14
//! title: a generic method taking a function-typed parameter invokes it and returns a closure over the result
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.14, 28.3
//! expect: run
//! output: 4600
//! notes: Harvested from the #9174 review (f_generic_apply). `Apply<T>` invokes `f` once and captures the result. Per iteration: k = 3; `Apply<int>(x + k, 4)` gives r = 7, and three calls of the returned closure add 21; `Apply<int>(x * 2, 1)` gives r = 2 and one call adds 2; 23 per iteration, 200 iterations: 4600.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Apply<T>(f: (T) -> T, seed: T): () -> T
    var r := f(seed);
    return lambda() => r;
  end Apply;
end Fl;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      using var fl := new Fl();
      var k := 3;
      var f := fl.Apply<int>(lambda(x: int) => x + k, 4);
      t := t + f() + f() + f();
      var g := fl.Apply<int>(lambda(x: int) => x * 2, 1);
      t := t + g();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
