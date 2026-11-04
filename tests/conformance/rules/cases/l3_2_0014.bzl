//! case: l3_2_0014
//! doc: stdlib
//! section: 3.2
//! title: GetString through an Encoding-typed value decodes what the same value's GetBytes encoded
//! rule: `GetString` raises `FormatException` when the input bytes are not a well-formed sequence in the encoding
//! cites: 3.2
//! expect: run
//! issue: 7028
//! output: True
//!   True
//!   True

module Conf;

public class Program
  public static method RoundTrips(e: Encoding, s: string): bool
    using var b := e.GetBytes(s);
    return e.GetString(b) = s;
  end RoundTrips;

  public static method Main()
    var s := "h😀é";
    Console.WriteLine(RoundTrips(Encoding.UTF8, s));
    Console.WriteLine(RoundTrips(Encoding.UTF16, s));
    Console.WriteLine(RoundTrips(Encoding.UTF32, s));
  end Main;
end Program;
