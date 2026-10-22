//! case: l3_2_0011
//! doc: stdlib
//! section: 3.2
//! title: the span-source encode forms keep the strict fault model of the string forms
//! rule: Stdlib §3.2: the span-source encode overloads carry "the same strict fault model, `FormatException` possible only
//!   for `ASCII`", and `GetBytes(s, destination)` raises `IndexOutOfRangeException` if the destination is too short.
//!   The window `"aé"` holds U+00E9, which `ASCII` cannot represent; the one-byte destination cannot hold `é€`.
//! cites: 3.2
//! expect: run
//! output: format-bytes
//!   format-count
//!   short
//!   3
module Conf;

public class Program
  public static method Main()
    var wide := new ReadOnlyStringSpan("aé", 0, 2);
    using var big := new Array<byte>(8, 0);
    try
      var n := Encoding.ASCII.GetBytes(wide, big.AsSpan());
      Console.WriteLine(n);
    catch FormatException =>
      Console.WriteLine("format-bytes");
    end;
    try
      var c := Encoding.ASCII.GetByteCount(wide);
      Console.WriteLine(c);
    catch FormatException =>
      Console.WriteLine("format-count");
    end;
    var rs := new ReadOnlyStringSpan("aé€z", 1, 2);
    using var small := new Array<byte>(1, 0);
    try
      var m := Encoding.UTF8.GetBytes(rs, small.AsSpan());
      Console.WriteLine(m);
    catch IndexOutOfRangeException =>
      Console.WriteLine("short");
    end;
    var ok := new ReadOnlyStringSpan("abc", 0, 3);
    Console.WriteLine(Encoding.ASCII.GetByteCount(ok));
  end Main;
end Program;
