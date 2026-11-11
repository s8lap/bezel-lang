//! case: s7_7_4707
//! doc: spec
//! section: 7.7
//! title: a nullable ?? chain with a differently-widened nullable right operand lifts the payload
//! rule: the result type is `T` when the right operand is non-nullable and `T?` when it is nullable
//! cites: 5.1, 7.7
//! expect: run
//! output: -100
//!   200
//!   -30000
//!   200
//!   True
//!   5
module Conf;

public class Program
  public static method Main()
    var l1: short? := null;
    var r1: sbyte? := sbyte(-100);
    var a1 := l1 ?? r1;
    Console.WriteLine((a1 ?? short(0)).ToString());
    var l2: short? := null;
    var r2: byte? := byte(200);
    var a2 := l2 ?? r2;
    Console.WriteLine((a2 ?? short(0)).ToString());
    var l3: long? := null;
    var r3: short? := short(-30000);
    var a3 := l3 ?? r3;
    Console.WriteLine((a3 ?? 0L).ToString());
    var l4: ulong? := null;
    var r4: byte? := byte(200);
    var a4 := l4 ?? r4;
    Console.WriteLine((a4 ?? 0UL).ToString());
    var l5: short? := null;
    var rn5: sbyte? := null;
    var b5 := l5 ?? rn5;
    Console.WriteLine(b5 is null);
    var l6: short? := short(5);
    var a6 := l6 ?? r1;
    Console.WriteLine((a6 ?? short(0)).ToString());
  end Main;
end Program;
