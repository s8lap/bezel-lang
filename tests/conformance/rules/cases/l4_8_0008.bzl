//! case: l4_8_0008
//! doc: stdlib
//! section: 4.8
//! title: GetOrNull returns null for an absent key
//! rule: GetOrNull(key: K): V?; // v0.231 (§1 *OrNull tier): absent -> null.
//! cites: 4.8, 1
//! expect: run
//! stdout: null
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    Console.WriteLine($"{d.GetOrNull("missing")}");
  end Main;
end Program;
