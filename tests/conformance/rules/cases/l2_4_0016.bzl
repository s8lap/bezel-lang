//! case: l2_4_0016
//! doc: stdlib
//! section: 2.4
//! title: Variables returns a dictionary whose iteration visits exactly Count entries
//! rule: `Variables()` returns a freshly-allocated `owned Dictionary<string, string>`
//! expect: run
//! stdout: True
//! issue: 7949
//! notes: The sentence fixes the result type, an owned Dictionary<string, string> bound here with using var. The iteration count equality is the Dictionary contract, independent of which variables the host has.
module Conf;

public class Program
  public static method Main()
    using var v := Environment.Variables();
    var seen := 0;
    for var kv in v do
      seen := seen + 1;
    end for;
    Console.WriteLine(seen = v.Count);
  end Main;
end Program;
