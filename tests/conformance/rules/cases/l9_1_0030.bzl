//! case: l9_1_0030
//! doc: stdlib
//! section: 9.1
//! title: int.Parse out of range raises ParseRangeException, which an IFailure arm takes
//! rule: whose input is well-formed and spells a value the type cannot represent
//!   raises `ParseRangeException`
//! cites: 9.1, 1
//! expect: run
//! stdout: True
//! issue: 8417
module Conf;

public class Program
  public static method Main()
    try
      _ := int.Parse("99999999999");
    catch
      var f: IFailure => Console.WriteLine(f is ParseRangeException)
    end;
  end Main;
end Program;
