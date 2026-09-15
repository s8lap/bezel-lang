//! case: s5_4_0005
//! doc: spec
//! section: 5.4
//! title: under duplicate constant values the declared-value set is the set of distinct values
//! rule: Under duplicate constant values (§10.2), the declared-value set is the
//!   set of distinct values: E(1) where Legacy := 1, Current := 1 yields that
//!   one value.
//! cites: 5.4, 10.2
//! expect: run
//! stdout: True
module Conf;

public enum Tier
  Legacy := 1, Current := 1;
end Tier;

public class Program
  public static method Main()
    var t := Tier(1);
    Console.WriteLine(t = Tier.Legacy);
  end Main;
end Program;
