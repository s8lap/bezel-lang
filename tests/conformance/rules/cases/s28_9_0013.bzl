//! case: s28_9_0013
//! doc: spec
//! section: 28.9
//! title: an owning collection is not trivial-dispose at any instantiation
//! rule: The owned-element variants of the standard collection types are not
//!   trivial-dispose at any instantiation: they own their elements, disposal
//!   cascades through to them, and their storage primitive declares a
//!   destructor — so the ordinary discharge obligations apply and the ceremony
//!   exemption does not (§28.9).
//! cites: 28.9, 28.11, 28.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class Ticket
  public property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;
end Ticket;

public class Program
  public static method Run()
    var pool := new OwningArray<Ticket>();
    Console.WriteLine(pool.Count);
  end Run;
end Program;
