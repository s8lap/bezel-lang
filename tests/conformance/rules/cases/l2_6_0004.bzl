//! case: l2_6_0004
//! doc: stdlib
//! section: 2.6
//! title: FromByteArray requires exactly 16 bytes
//! rule: Guid.FromByteArray(bytes) requires Count = 16, raising
//!   ArgumentException otherwise.
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      var bytes := new Array<byte>(4, 0);
      _ := Guid.FromByteArray(bytes);
      Console.WriteLine("no exception");
    catch
      ArgumentException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
