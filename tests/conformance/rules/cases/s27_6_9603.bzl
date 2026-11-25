//! case: s27_6_9603
//! doc: spec
//! section: 27.6
//! title: a field overwritten with bare instance-method names holds the last one
//! rule: Method references work for static methods, instance methods (the instance
//!   is captured at formation — by reference for class- or interface-typed
//!   receivers, by copy for value-shaped receivers
//! cites: 27.6, 2.7
//! expect: run
//! stdout: 700
//! issue: 9216
module Conf;

public class P
  private cb: () -> int := lambda() => 0;

  public method Get(): int => 5;
  public method Get2(): int => 7;

  public method Arm()
    this.cb := Get;
    this.cb := Get;
    this.cb := Get2;
  end Arm;

  public method Make(): int => cb();

  public static method Main()
    var total := 0;
    for var j in 0 ..< 100 do
      var p := new P();
      p.Arm();
      var v := p.Make();
      total := total + v;
    end for;
    Console.WriteLine(total);
  end Main;
end P;
