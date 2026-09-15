//! case: l4_17_0002
//! doc: stdlib
//! section: 4.17
//! title: Add on a present key raises ArgumentException
//! rule: method Add(key: K, value: V); // v0.282: ArgumentException if the
//!   key is present -- the duplicate-is-a-bug tier.
//! cites: 4.17
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var sd := new SortedDictionary<int, string>();
    sd.Add(1, "a");
    try
      sd.Add(1, "b");
    catch
      ArgumentException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
