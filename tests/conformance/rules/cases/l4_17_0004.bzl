//! case: l4_17_0004
//! doc: stdlib
//! section: 4.17
//! title: FirstKey on an empty SortedDictionary raises InvalidOperationException
//! rule: pure readonly property FirstKey: K; // least key;
//!   InvalidOperationException if empty.
//! cites: 4.17
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var sd := new SortedDictionary<int, string>();
    try
      var k := sd.FirstKey;
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
