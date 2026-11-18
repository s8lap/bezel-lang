//! case: s16_4_6459
//! doc: spec
//! section: 16.4
//! title: a uint step variable holding zero raises OverflowException at the range
//!   expression
//! rule: evaluating a range expression whose computed step is zero raises
//!   `OverflowException` at the expression
//! cites: 16.4
//! expect: run
//! stdout: caught
module Conf;
public class Program
  public static method Show(xs: IEnumerable<uint>): string
    var s := "";
    for var x in xs do s := s + $"{x},"; end;
    return $"[{s}]";
  end Show;
  public static method Main()
    var u0: uint := 0;
    var u9: uint := 4000000000;
    var st: uint := 3000000000;
    var z: uint := 0;
    try
      Console.WriteLine(Show(u0..u9 step z));
    catch
      OverflowException => Console.WriteLine("caught"),
    end try;
  end Main;
end Program;
