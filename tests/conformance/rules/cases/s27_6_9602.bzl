//! case: s27_6_9602
//! doc: spec
//! section: 27.6
//! title: a local reassigned among bare instance-method names invokes the last one
//! rule: Method references work for static methods, instance methods (the instance
//!   is captured at formation — by reference for class- or interface-typed
//!   receivers, by copy for value-shaped receivers
//! cites: 27.6, 2.7
//! expect: run
//! stdout: 1200
//! issue: 9216
module Conf;

public class P
  public method Get(): int => 5;
  public method Get2(): int => 7;

  public method Make(): int
    var f: () -> int := Get;
    var a := f();
    f := Get;
    f := Get2;
    var b := f();
    return a + b;
  end Make;

  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      var p := new P();
      var v := p.Make();
      total := total + v;
    end for;
    Console.WriteLine(total);
  end Main;
end P;
