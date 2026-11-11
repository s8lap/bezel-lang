//! case: l9_1_4614
//! doc: stdlib
//! section: 9.1
//! title: long and ulong ParseOrNull are null outside the range and the value at ulong.MaxValue
//! rule: a value outside the type's range raising `ParseRangeException`
//! cites: 9.1, 1
//! expect: run
//! stdout: null null 18446744073709551615 null
module Conf;

public class Program
  public static method Main()
    var g := long.ParseOrNull("99999999999999999999");
    var h := ulong.ParseOrNull("18446744073709551616");
    var k := ulong.ParseOrNull("18446744073709551615");
    var m := long.ParseOrNull("-9223372036854775809");
    Console.WriteLine($"{g} {h} {k} {m}");
  end Main;
end Program;
