//! case: l4_4_0002
//! doc: stdlib
//! section: 4.4
//! title: Array<T>(size, fill) fills every slot with the given value
//! rule: constructor(size: int, fill: T); // Count = size, every slot =
//!   fill (v0.165, #1375)
//! cites: 4.4
//! expect: run
//! output: 5
//!   5
//!   5
module Conf;

public class Program
  public static method Main()
    using var arr := new Array<int>(3, 5);
    Console.WriteLine(arr[0]);
    Console.WriteLine(arr[1]);
    Console.WriteLine(arr[2]);
  end Main;
end Program;
