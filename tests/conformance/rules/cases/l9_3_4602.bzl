//! case: l9_3_4602
//! doc: stdlib
//! section: 9.3
//! title: decimal.ParseOrNull rounds to MaxValue at .4 and is null at .5 beyond the range
//! rule: so `"79228162514264337593543950335.4"` parses to `MaxValue` and `"79228162514264337593543950335.5"` raises
//! cites: 9.3
//! expect: run
//! stdout: 79228162514264337593543950335 null
module Conf;

public class Program
  public static method Main()
    var a := decimal.ParseOrNull("79228162514264337593543950335.4");
    var b := decimal.ParseOrNull("79228162514264337593543950335.5");
    Console.WriteLine($"{a} {b}");
  end Main;
end Program;
