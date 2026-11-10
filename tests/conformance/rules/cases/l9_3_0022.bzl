//! case: l9_3_0022
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse out of range raises ParseRangeException, which an IFailure arm takes
//! rule: a value outside `decimal`'s range … raises `ParseRangeException` (§9.1).
//!   `TryParse` returns `false` in every case where `Parse` would raise.
//! cites: 9.3, 9.1
//! expect: run
//! stdout: True
//! issue: 8417
module Conf;

public class Program
  public static method Main()
    try
      _ := decimal.Parse("79228162514264337593543950336");
    catch
      var f: IFailure => Console.WriteLine(f is ParseRangeException)
    end;
  end Main;
end Program;
