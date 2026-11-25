//! case: s27_6_9601
//! doc: spec
//! section: 27.6
//! title: a bare instance-method name returned as a function value keeps the instance
//! rule: Method references work for static methods, instance methods (the instance
//!   is captured at formation — by reference for class- or interface-typed
//!   receivers, by copy for value-shaped receivers
//! cites: 27.6, 2.7, 28.9
//! expect: run
//! stdout: 1000
//! issue: 9216
module Conf;

public class P
  public method Get(): int => 5;

  public method MakeF(): () -> int
    return Get;
  end MakeF;

  public method MakeG(): () -> int => Get;

  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      var p := new P();
      var f := p.MakeF();
      var g := p.MakeG();
      var a := f();
      var b := g();
      total := total + a + b;
    end for;
    Console.WriteLine(total);
  end Main;
end P;
