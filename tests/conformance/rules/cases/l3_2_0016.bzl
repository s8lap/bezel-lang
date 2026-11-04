//! case: l3_2_0016
//! doc: stdlib
//! section: 3.2
//! title: an Encoding-typed UTF8 value decodes a byte span and raises FormatException on a truncated tail
//! rule: Ill-formed sequences — overlong forms, unpaired-surrogate encodings, truncated tails — raise `FormatException`.
//! cites: 3.2
//! expect: run
//! issue: 7028
//! output: True
//!   fmt

module Conf;

public class Program
  public static method Main()
    var u: Encoding := Encoding.UTF8;
    using var bytes := Encoding.UTF8.GetBytes("é");
    Console.WriteLine(u.GetString(bytes.AsReadOnlySpan(0, 2)) = "é");
    try
      Console.WriteLine(u.GetString(bytes.AsReadOnlySpan(0, 1)));
    catch
      FormatException => Console.WriteLine("fmt");
    end;
  end Main;
end Program;
