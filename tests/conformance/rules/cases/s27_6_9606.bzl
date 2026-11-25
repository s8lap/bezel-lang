//! case: s27_6_9606
//! doc: spec
//! section: 27.6
//! title: a bare instance-method name in an interface implementation invokes the instance
//! rule: Method references work for static methods, instance methods (the instance
//!   is captured at formation — by reference for class- or interface-typed
//!   receivers, by copy for value-shaped receivers
//! cites: 27.6, 2.7
//! expect: run
//! stdout: 1000
//! issue: 9216
module Conf;

public interface IA
  method Get(): int;
  method Make(): int;
end IA;

public class P: IA
  public method Get(): int => 5;

  public method Make(): int
    var f: () -> int := Get;
    var g: () -> int := Get;
    var a := f();
    var b := g();
    return a + b;
  end Make;
end P;

public class Program
  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      var p := new P();
      var i: IA := p;
      var v := i.Make();
      total := total + v;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
