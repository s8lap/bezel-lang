//! case: l9_1_4601
//! doc: stdlib
//! section: 9.1
//! title: sbyte/byte/ushort/uint ParseOrNull is null for a value outside the type's range
//! rule: a value outside the type's range raising `ParseRangeException`
//! cites: 9.1, 1
//! expect: run
//! stdout: null null null null
module Conf;

public class Program
  public static method Main()
    var a := sbyte.ParseOrNull("128");
    var b := byte.ParseOrNull("256");
    var c := ushort.ParseOrNull("-1");
    var d := uint.ParseOrNull("-5");
    Console.WriteLine($"{a} {b} {c} {d}");
  end Main;
end Program;
