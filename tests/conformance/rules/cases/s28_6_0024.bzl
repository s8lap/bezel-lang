//! case: s28_6_0024
//! doc: spec
//! section: 28.6
//! title: disposing a borrowed owning collection's container is rejected
//! rule: A borrow of an owning collection may not be disposed: the borrower
//!   does not own the container, and the actual owner disposes the container
//!   and cascades to its elements at scope/destructor end.
//! cites: 28.6
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Cleanup(pool: OwningArray<Widget>)
    pool.Dispose();
  end Cleanup;
end Program;
