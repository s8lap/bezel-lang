//! case: s29_8_0029
//! doc: spec
//! section: 29.8
//! title: a T-typed iterator parameter at an owning-choice instantiation makes the result single-consumption
//! rule: a parameter whose declared type is or contains a type parameter
//!   counting as move-only at every instantiation whose substitution makes
//!   that type move-only (§3.6 rule 2; §15.3's parameter rule), so
//!   `Each<T>(x: T)` mints a single-consumption iterable at `Each<Slot>` for
//!   an owning choice `Slot` and a re-iterable one at `Each<int>`, the static
//!   check below applied at a call site that names such an instantiation
//! cites: 29.8, 29.11, 15.3
//! expect: reject
//! diagnostic: ToArray
//! issue: 5902
module Conf;

public class Conn
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Conn;

public choice Slot
  Held(owned c: Conn),
  Empty;
end Slot;

public class Program
  public static method Each<T>(x: T): owned IEnumerable<int>
    yield return 1;
  end Each;

  public static method Main()
    using var xs := Each<Slot>(Slot.Held(new Conn()));
    for var a in xs do
      Console.WriteLine(a);
    end;
    for var b in xs do
      Console.WriteLine(b);
    end;
  end Main;
end Program;
