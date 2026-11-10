//! case: s31_4_0012
//! doc: spec
//! section: 31.4
//! title: the FormatException int.Parse raises is taken by an IFailure arm
//! rule: Of the built-in types, `FormatException` implements `IFailure`.
//! cites: 31.4, 31.2.1
//! expect: run
//! stdout: failure
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    try
      _ := int.Parse("12x");
    catch
      IFailure => Console.WriteLine("failure")
    end;
  end Main;
end Program;
