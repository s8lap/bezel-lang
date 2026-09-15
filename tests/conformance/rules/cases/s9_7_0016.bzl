//! case: s9_7_0016
//! doc: spec
//! section: 9.7
//! title: a record-carried borrow whose source outlives the collection may be copied in
//! rule: A borrow-bearing record value may be copied into a collection when every
//!   carried borrow's source provably outlives the collection's own binding
//!   scope; the check is the same structural scope comparison as section 28.3's
//!   borrow rules.
//! cites: 28.11, 28.3
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Handle(Conn: Connection);

public class Program
  public static method Collect()
    var c := new Connection();
    using var handles: Array<Handle> := [];
    handles.Add(new Handle(c));
  end Collect;
end Program;
