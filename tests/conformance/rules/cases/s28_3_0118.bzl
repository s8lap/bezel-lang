//! case: s28_3_0118
//! doc: spec
//! section: 28.3
//! title: a capture-bearing function value as the receiver of an extension on Object is rejected
//! rule: as the receiver of an extension call whose head is `Object` — which
//!   §26.5 rule 2 reaches from a function-typed receiver, a function type
//!   reaching `Object` through its inheritance chain (§3.1) — it is, the twin
//!   of §9.7's *borrow-erasing position* clause for the receiver of such a
//!   call. At every set-erasing position **a function value with a non-empty
//!   source set is rejected, and a capture-free one is admitted**
//! cites: 28.3, 26.5
//! expect: reject
//! diagnostic: set-erasing
//! issue: 5514
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Keeper
  private _o: Object?;

  public constructor()
  end constructor;

  public method Put(o: Object)
    _o := o;
  end Put;
end Keeper;

public extension Object
  public method Leak(k: Keeper) => k.Put(this);
end extension;

public class Client
  public static method Run(p: Item, k: Keeper)
    var f := lambda() => p.Size();
    f.Leak(k);
  end Run;
end Client;
