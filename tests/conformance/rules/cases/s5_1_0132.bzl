//! case: s5_1_0132
//! doc: spec
//! section: 5.1
//! title: every integer type and char converts to decimal preserving its value
//! rule: The conversion table lists `decimal` among the implicit targets of `sbyte`, `byte`,
//!   `short`, `ushort`, `int`, `uint`, `long`, `ulong` and `char` (§5.1, v0.340).
//!   `Math.Max<decimal>` binds its type argument explicitly, so each argument converts to
//!   `decimal`; a `decimal` renders its exact digits (stdlib §9.3).
//! cites: stdlib 9.3
//! expect: run
//! output: 18446744073709551615
//!   -128
//!   65535
//!   100
//!   -9223372036854775807
module Conf;

public class Program
  public static method Main()
    var um: ulong := 18446744073709551615UL;
    var sm: sbyte := -128;
    var sh: short := -32768;
    var ush: ushort := 65535;
    var bb: byte := 255;
    var ch: char := 'a';
    var ch2: char := 'd';
    var lm: long := -9223372036854775807;
    Console.WriteLine(Math.Max<decimal>(um, bb));
    Console.WriteLine(Math.Min<decimal>(sm, bb));
    Console.WriteLine(Math.Max<decimal>(sh, ush));
    Console.WriteLine(Math.Max<decimal>(ch, ch2));
    Console.WriteLine(Math.Min<decimal>(lm, sh));
  end Main;
end Program;
