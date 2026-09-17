//! case: s9_7_0015
//! doc: spec
//! section: 9.7
//! title: a record-carried borrow may not be copied into a longer-lived collection
//! rule: A borrow-bearing record value may be copied into a collection only when
//!   every carried borrow's source provably outlives the collection's own
//!   binding scope; a store into a collection whose binding's scope strictly
//!   encloses a carried source's scope is rejected at the store site.
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlives
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection);

public class Program
  public static method Collect()
    using var handles: Array<Handle> := [];
    do
      var c := new Connection();
      handles.Add(new Handle(c));
    end;
  end Collect;
end Program;
