//! case: l4_8_0002
//! doc: stdlib
//! section: 4.8
//! title: Dictionary<K,V>.Add raises ArgumentException on a duplicate key
//! rule: Add(key, value) inserts and raises ArgumentException if the key is
//!   already present.
//! cites: 4.8
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    d.Add("a", 1);
    try
      d.Add("a", 2);
    catch
      ArgumentException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
