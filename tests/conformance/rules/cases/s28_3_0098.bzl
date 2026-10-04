//! case: s28_3_0098
//! doc: spec
//! section: 28.3
//! title: a call-bounded callback stored through a local indexer is rejected
//! rule: A property or indexer assignment of a function-typed value with a
//!   non-empty source set is rejected at the site (spec §28.3, v0.314).
//!   A local indexer store (items[i] := cb where items is a local variable)
//!   was previously missed because the existing IndexExpr arm only checked
//!   collection reachability, not whether the indexer's set accessor itself
//!   is a capturing sink.
//! cites: 28.3, 28.14
//! expect: reject
//! diagnostic: call-bounded
module Conf;

public class MyList
  private _items: Array<() -> ()>;

  public constructor()
    _items := new Array<() -> ()>();
  end constructor;

  public property [i: int]: () -> ()
    get => _items[i];
    set => _items[i] := value;
  end;
end MyList;

public class C
  public method Store(cb: () -> ())
    var items: MyList := new MyList();
    items[0] := cb;
  end Store;
end C;
