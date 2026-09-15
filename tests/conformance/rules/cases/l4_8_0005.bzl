//! case: l4_8_0005
//! doc: stdlib
//! section: 4.8
//! title: dict[key] += 1 raises on an absent key; the compound form never inserts
//! rule: dict[key] += 1 desugars to a getter plus a setter, and the getter
//!   raises KeyNotFoundException on an absent key — the compound form
//!   updates, it never inserts.
//! cites: 4.8
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var counts := new Dictionary<string, int>();
    try
      counts["missing"] += 1;
    catch
      KeyNotFoundException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
