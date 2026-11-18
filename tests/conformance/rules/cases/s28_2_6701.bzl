//! case: s28_2_6701
//! doc: spec
//! section: 28.2
//! title: a closure over the variable itself is invoked after a reassignment lifts the kill
//! rule: a reassignment between the kill and the invocation lifts it as it lifts any other read of the variable
//! cites: 28.3, 27.5
//! expect: run
//! stdout: 2
module Conf;

public class Res
  private _n: int;
  public constructor(n: int)
    _n := n;
  end constructor;
  public method Ping(): int => _n;
end Res;

public class Program
  public static method Run(): int
    var d := new Res(1);
    var h := lambda() => d.Ping();
    d.Dispose();
    d := new Res(2);
    return h();
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
