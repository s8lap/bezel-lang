//! case: s28_11_0002
//! doc: spec
//! section: 28.11
//! title: storing a scope-bound local's borrow into an outliving field-held collection is rejected
//! rule: A borrow of a scope-bound binding — an owned parameter included —
//!   stored into a collection that provably outlives that scope — the
//!   collection is a field, is reachable from one, escapes the method, or
//!   is a local of strictly wider scope — is rejected at the store site.
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  private _items: Array<Widget>;

  public constructor()
    _items := new Array<Widget>();
  end constructor;

  public method TrackFresh()
    using var w := new Widget();
    _items.Add(w);
  end TrackFresh;
end Registry;
