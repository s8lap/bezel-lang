//! case: s22_5_4505
//! doc: spec
//! section: 22.5
//! title: reordered named arguments to int.Parse work as call arguments, coalesce
//!   operands and loops
//! rule: naming an argument changes which parameter it binds to, never when it runs
//! cites: 22.6
//! expect: run
//! output: 32
//!   15
//!   500
//!   65297
//!   7
//! issue: 7602
module Conf;

class C
  public static method Twice(x: int): int
    return x * 2;
  end Twice;
  public static method Main(): int
    Console.WriteLine(Twice(int.Parse(radix := 16, s := "10")));
    var n: int? := null;
    Console.WriteLine(n ?? int.Parse(radix := 8, s := "17"));
    var sum: int := 0;
    for var i in 0..<500 do
      sum := sum + int.Parse(radix := 16, s := "1");
    end;
    Console.WriteLine(sum);
    var s1: string := "ff";
    var s2: string := "11";
    Console.WriteLine(int.Parse(radix := 16, s := s1 + s2));
    Console.WriteLine(int.Parse(s := "7", radix := int.Parse(radix := 10, s := "8")));
    return 0;
  end Main;
end C;
