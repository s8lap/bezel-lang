//! case: l4_19_0006
//! doc: stdlib
//! section: 4.19
//! title: a static owned readonly FrozenArray<T> is a sound all-domains singleton
//! rule: A static owned readonly FrozenArray<T> is a sound all-domains
//!   singleton and draws no language spec 22.12 rule-4 warning (rule 4's
//!   warning-suppression set, the v0.186 ruling); mutable statics of it
//!   remain rejected.
//! cites: 4.19, 22.12, 28.13
//! expect: run
//! stdout: 2
module Conf;

public class Program
  private static owned readonly _names: FrozenArray<string> :=
    FrozenArray<string>.From(["a", "b"]);

  public static method Main()
    Console.WriteLine(_names.Length);
  end Main;
end Program;
