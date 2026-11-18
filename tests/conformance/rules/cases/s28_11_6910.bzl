//! case: s28_11_6910
//! doc: spec
//! section: 28.11
//! title: an element read through a field path stays reported at its store into a purely
//!   local collection
//! rule: An element read through a field path stays reported at its store into such a
//!   collection too
//! cites: 28.11, 28.3
//! expect: warn
//! diagnostic: borrowing-collection store
//! flags: --report-trust-admissions
//! issue: 7468
//! notes: The exemption is conditioned on the collection not escaping the body and on the
//!   store naming the binding itself; the stores below name the plain parameter's borrow,
//!   which is reported at its store.
module Conf;
public class Item
  public constructor() end constructor;
end Item;
public class Holder
  public owned _child: Item;
  public constructor()
    _child := new Item();
  end constructor;
end Holder;
public class Program
  public static method Register(h: Holder)
    using var tmp := new Array<Item>();
    tmp.Add(h._child);
  end Register;
end Program;
