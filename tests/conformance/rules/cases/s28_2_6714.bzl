//! case: s28_2_6714
//! doc: spec
//! section: 28.2
//! title: invoking a closure over the variable in a loop after the kill is an error
//! rule: A closure that captures the variable itself, rather than a holder of its value, reads the variable when it is invoked
//! cites: 28.3, 27.5
//! expect: reject
//! diagnostic: dominat
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
    var s := 0;
    var i := 0;
    while i < 3 do
      s := s + h();
      i := i + 1;
    end while;
    return s;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
