//! case: l5_3602
//! doc: stdlib
//! section: 5
//! title: SingleOrNull(pred) with two matches raises InvalidOperationException
//! rule: `FirstOrNull(pred)`/`LastOrNull(pred)`/`SingleOrNull(pred)` return `null` when none matches
//!   (`SingleOrNull(pred)` still raises when more than one matches)
//! cites: 5, 1
//! expect: run
//! stdout: raised
//! issue: 9287
module Conf;

public class Program
  public static method Main()
    using var a: Array<int> := [1, 2];
    try
      var r := a.SingleOrNull(lambda(x: int) => x > 0);
      Console.WriteLine("no raise");
    catch
      InvalidOperationException => Console.WriteLine("raised"),
    end try;
  end Main;
end Program;
