//! case: l4_8_0001
//! doc: stdlib
//! section: 4.8
//! title: Dictionary<K,V> indexer raises KeyNotFoundException on a missing key
//! rule: Indexer dict[key] raises KeyNotFoundException if the key is not
//!   present.
//! cites: 4.8
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    try
      _ := d["missing"];
    catch
      KeyNotFoundException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
