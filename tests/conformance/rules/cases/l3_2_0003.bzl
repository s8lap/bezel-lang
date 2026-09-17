//! case: l3_2_0003
//! doc: stdlib
//! section: 3.2
//! title: UTF8.GetString raises FormatException on malformed bytes
//! rule: Malformed or unrepresentable input is a defined fault — GetString
//!   raises FormatException when the input bytes are not a well-formed
//!   sequence; no replacement characters are produced.
//! cites: 3.2
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var bad: Array<byte> := [255, 254];
    try
      _ := Encoding.UTF8.GetString(bad);
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
