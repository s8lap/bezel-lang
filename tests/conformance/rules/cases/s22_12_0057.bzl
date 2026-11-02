//! case: s22_12_0057
//! doc: spec
//! section: 22.12
//! title: a mutable static plain int inline array draws the value-static warning
//! rule: **(iii) Warned value static:** a mutable static whose declared type is a
//!   **primitive value type** ([§3.1](#sec-type-kinds)) or an **enum type**, the
//!   nullable form of either, or — fields only — a **plain inline array**
//!   (`[N]T` without the `owned` slot marker, [§15.1](#sec-inline-arrays)) whose
//!   element type is itself, recursively, one of these.
//! cites: 22.12, 15.1
//! expect: warn
//! diagnostic: static
module Conf;

public class Registry
  public static Pool: [2]int := [1, 2];
end Registry;

public class Program
  public static method Main(): int
    Registry.Pool[0] := 5;
    Console.WriteLine(Registry.Pool[0].ToString());
    return 0;
  end Main;
end Program;
