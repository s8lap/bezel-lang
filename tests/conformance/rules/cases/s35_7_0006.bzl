//! case: s35_7_0006
//! doc: spec
//! section: 35.7
//! title: a direct Equals call on an owning choice that declares no such member is rejected
//! rule: A choice that is **not** auto-hashable — any borrow-bearing or owning
//!   choice — draws §9.2's use-site compile error on `=`, `<>`, and direct
//!   `Equals`/`GetHashCode` calls, unless the author declares the member
//!   itself.
//! cites: 35.7, 9.2
//! expect: reject
//! diagnostic: auto-hashable
//! issue: 4391
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Holder
  Full(owned w: Widget),
  Empty;
end Holder;

public class Program
  public static method Main()
    using var a := Holder.Full(new Widget());
    using var b := Holder.Full(new Widget());
    Console.WriteLine(a.Equals(b));
  end Main;
end Program;
