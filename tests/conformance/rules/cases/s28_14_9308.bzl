//! case: s28_14_9308
//! doc: spec
//! section: 28.14
//! title: an override returning a closure over the invoked callback value dispatches through the base
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.14, 28.3
//! expect: run
//! output: 43000
//! notes: Harvested from the #9174 review (j_virtual). `A.Get` invokes `q` and returns a closure over the value; `B.Get` overrides it adding 100. Per iteration x = 5: through a base-typed reference to a `B`, v = 5 and the closure yields 105, called twice: 210; through an `A`, 5: 215 per iteration, 200 iterations: 43000.
module Conf;
public class A
  public constructor() end constructor;
  public virtual method Get(q: () -> int): () -> int
    var v := q();
    return lambda() => v;
  end Get;
end A;
public class B: A
  public constructor() end constructor;
  public override method Get(q: () -> int): () -> int
    var v := q();
    return lambda() => v + 100;
  end Get;
end B;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var x := 5;
      using var a: A := new B();
      var f := a.Get(lambda() => x);
      t := t + f() + f();
      using var a2: A := new A();
      var f2 := a2.Get(lambda() => x);
      t := t + f2();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
