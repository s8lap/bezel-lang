//! case: s35_7_0005
//! doc: spec
//! section: 35.7
//! title: direct Equals and GetHashCode calls are admitted on an owning choice that declares them
//! rule: The exemption for **direct `Equals`/`GetHashCode` calls** is keyed on
//!   the declared **members**, not on the operators: a choice that declares
//!   plain `Equals(other: C): bool` and `GetHashCode(): int` admits those calls
//!   on **every** classification, owning and borrow-bearing included — they
//!   resolve statically to the declared members.
//! cites: 35.7, 35.2, 25.2
//! expect: run
//! output: False
//!   7
//! issue: 4391
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Holder
  Full(owned w: Widget),
  Empty;

  public method Equals(other: Holder): bool => false;
  public method GetHashCode(): int => 7;
end Holder;

public class Program
  public static method Main()
    using var a := Holder.Full(new Widget());
    using var b := Holder.Full(new Widget());
    Console.WriteLine(a.Equals(b));
    Console.WriteLine(a.GetHashCode().ToString());
  end Main;
end Program;
