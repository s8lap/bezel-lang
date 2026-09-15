//! case: l4_4_0001
//! doc: stdlib
//! section: 4.4
//! title: Array<T>(capacity) reserves storage without adding elements
//! rule: constructor(capacity: int); // empty (Count = 0), storage reserved
//! cites: 4.4
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    using var arr := new Array<int>(16);
    Console.WriteLine(arr.Count);
  end Main;
end Program;
