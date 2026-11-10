//! case: l9_2_0020
//! doc: stdlib
//! section: 9.2
//! title: double.Parse out of range raises ParseRangeException, which an IFailure arm takes
//! rule: raises `ParseRangeException` (§9.1): the type cannot represent the number
//!   the input spells
//! cites: 9.2, 9.1
//! expect: run
//! stdout: True
//! issue: 8417
module Conf;

public class Program
  public static method Main()
    try
      _ := double.Parse("1e999");
    catch
      var f: IFailure => Console.WriteLine(f is ParseRangeException)
    end;
  end Main;
end Program;
