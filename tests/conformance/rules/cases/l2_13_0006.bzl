//! case: l2_13_0006
//! doc: stdlib
//! section: 2.13
//! title: EnumNames on a non-enum Type raises InvalidOperationException
//! rule: Enum metadata (EnumNames, EnumValues) is valid only when IsEnum; on
//!   any other Type, accessing either property raises
//!   InvalidOperationException.
//! cites: 2.13
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      _ := typeof(int).EnumNames;
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
