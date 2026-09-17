//! case: s15_1_2_0001
//! doc: spec
//! section: 15.1.2
//! title: an inline array's Length is a compile-time constant N
//! rule: Every inline array `[N]T` exposes `arr.Length: int` — a compile-time
//!   constant N (§15.1.2).
//! cites: 15.1.2
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var arr: [5]int;
    Console.WriteLine(arr.Length);
  end Main;
end Program;
