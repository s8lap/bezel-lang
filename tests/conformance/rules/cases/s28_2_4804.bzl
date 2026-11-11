//! case: s28_2_4804
//! doc: spec
//! section: 28.2
//! title: a closure over a variable disposed and then reassigned is invoked without error
//! rule: A closure that captures the variable itself, rather than a holder of its value, reads the variable when it is invoked ([§27.5](#sec-closures)): its invocation after the kill is a use of the variable under the first sentence, and a reassignment between the kill and the invocation lifts it as it lifts any other read of the variable.
//! cites: 28.3, 27.6
//! expect: run
//! output: 1800
module Conf;

public class D
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;

  public method Ping(): int => _n;
  public static method Stat(): int => 5;
end D;

public class C
  public static method Run(k: bool): int
    var d := new D(7);
    var g := lambda() => d.Ping();
    d.Dispose();
    d := new D(9);
    return g();
  end Run;

  public static method Main()
    var total := 0;
    var i := 0;
    while i < 200 do
      total := total + C.Run(i >= 0);
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end C;
