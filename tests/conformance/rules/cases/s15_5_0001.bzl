//! case: s15_5_0001
//! doc: spec
//! section: 15.5
//! title: fixed extent uses Length while resizable extent uses Count
//! rule: One rule assigns the member name: fixed extent uses `.Length`;
//!   resizable uses `.Count` (§15.5).
//! cites: 15.5
//! expect: run
//! stdout: 4
module Conf;

public class Program
  public static method Main()
    var fixedArr: [4]int;
    using var heapArr := new Array<int>();
    Console.WriteLine(fixedArr.Length);
    Console.WriteLine(heapArr.Count);
  end Main;
end Program;
