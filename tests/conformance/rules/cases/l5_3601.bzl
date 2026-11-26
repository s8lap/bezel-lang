//! case: l5_3601
//! doc: stdlib
//! section: 5
//! title: SingleOrNull() over a source of two elements raises InvalidOperationException
//! rule: The `*OrNull` variant answers `null` for the missing element only — the band-2 *Expected absence*
//!   its tier is for — so `SingleOrNull()` returns `null` on an empty source and, like `Single()`, raises
//!   `InvalidOperationException` when the source holds more than one element
//! cites: 5, 1
//! expect: run
//! stdout: raised
//! issue: 9287
module Conf;

public class Program
  public static method Main()
    using var a: Array<int> := [1, 2];
    try
      var r := a.SingleOrNull();
      Console.WriteLine("no raise");
    catch
      InvalidOperationException => Console.WriteLine("raised"),
    end try;
  end Main;
end Program;
