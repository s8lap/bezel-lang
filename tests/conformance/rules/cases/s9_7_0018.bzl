//! case: s9_7_0018
//! doc: spec
//! section: 9.7
//! title: an owned T factory whose fresh result carries no borrow is admitted at a borrow-bearing record
//! rule: The rejection is per return expression, never at the declaration ... In
//!   a generic body this bullet reaches the return of a member declared to
//!   return `owned T` for a bare type parameter `T` ... it is checked at each
//!   instantiation substituting for it a value type that holds borrows
//! cites: 9.7, 28.1, 14.2
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public record Opt
  public readonly property Ref: Item?;

  public constructor()
    Ref := null;
  end constructor;
end Opt;

public class Fns
  public static method Make<T>(): owned T where T: new() => new T();
end Fns;

public class Client
  public static method Run(): int
    var o := Fns.Make<Opt>();
    return (if o.Ref = null then 0 else 1);
  end Run;
end Client;
