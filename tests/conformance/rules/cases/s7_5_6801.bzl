//! case: s7_5_6801
//! doc: spec
//! section: 7.5
//! title: a `?.` call of a user method named like a built-in property still dispatches to the method
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 26.5
//! expect: run
//! output: 7
//!   True
//!   3
//! notes: Over-rejection control for #8650: the receiver's declared type decides (26.5 rule 1), and Tally declares
//!   methods, not properties, named Count, First and IsReadOnly.
module Conf;

public class Tally
  public constructor() end constructor;
  public method Count(): int => 7;
  public method IsReadOnly(): bool => true;
  public method First(): int => 3;
end Tally;

public class Program
  public static method Main()
    using var t: Tally? := new Tally();
    Console.WriteLine(t?.Count() ?? -1);
    Console.WriteLine(t?.IsReadOnly() ?? false);
    Console.WriteLine(t?.First() ?? -1);
  end Main;
end Program;
