//! case: s27_6_9605
//! doc: spec
//! section: 27.6
//! title: a bare virtual-method name invokes the override
//! rule: When a lambda's body is just a call to an existing method, the method
//!   reference may be used directly
//! cites: 27.6, 2.7
//! expect: run
//! stdout: 2200
//! issue: 9216
module Conf;

public class B
  public virtual method Get(): int => 5;

  public method Make(): int
    var f: () -> int := Get;
    var g: () -> int := Get;
    var a := f();
    var b := g();
    return a + b;
  end Make;
end B;

public class P: B
  public override method Get(): int => 11;
end P;

public class Program
  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      var p := new P();
      var v := p.Make();
      total := total + v;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
