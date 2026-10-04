//! case: s28_14_0022
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback stored through a local indexer is rejected
//! rule: A function-typed parameter declared without scoped is call-bounded;
//!   the callback may not be stored in a field of any object (spec §28.14).
//!   A local indexer store (items[i] := cb where items is a local variable)
//!   was previously missed because the existing IndexExpr arm only checked
//!   collection reachability, not whether the indexer's set accessor itself
//!   is a capturing sink.
//! cites: 28.14
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
