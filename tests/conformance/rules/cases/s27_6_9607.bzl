//! case: s27_6_9607
//! doc: spec
//! section: 27.6
//! title: a conditional over two bare instance-method names invokes the chosen one
//! rule: Method references work for static methods, instance methods (the instance
//!   is captured at formation — by reference for class- or interface-typed
//!   receivers, by copy for value-shaped receivers
//! cites: 27.6, 2.7, 20
//! expect: run
//! stdout: 1302
//! issue: 9216
module Conf;

public class P
  public method Get(): int => 5;
  public method Get2(): int => 7;

  public method Make(c: bool): int
    var f: () -> int := (if c then Get else Get2);
    return f();
  end Make;
end P;

public class Program
  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      var p := new P();
      var a := p.Make(j > 50);
      var b := p.Make(false);
      total := total + a + b;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
