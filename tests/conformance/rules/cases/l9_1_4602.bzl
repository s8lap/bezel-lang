//! case: l9_1_4602
//! doc: stdlib
//! section: 9.1
//! title: sbyte/byte/ushort/uint ParseOrNull returns the value at the type's own bounds
//! rule: an optional leading `+` or `-`, then one or more digits of that radix, and nothing else
//! cites: 9.1, 1
//! expect: run
//! stdout: 127 -128 255 65535 4294967295
module Conf;

public class Program
  public static method Main()
    var a := sbyte.ParseOrNull("127");
    var b := sbyte.ParseOrNull("-128");
    var c := byte.ParseOrNull("255");
    var d := ushort.ParseOrNull("65535");
    var e := uint.ParseOrNull("4294967295");
    Console.WriteLine($"{a} {b} {c} {d} {e}");
  end Main;
end Program;
