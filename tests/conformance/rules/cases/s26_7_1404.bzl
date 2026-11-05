//! case: s26_7_1404
//! doc: spec
//! section: 26.7
//! title: an extension cannot store its receiver returned by a self-returning call through a local
//! rule: The receiver is always a borrow; extensions cannot store `this`, return it as `owned`, or
//!   dispose it.
//! cites: 26.7, 28.3
//! expect: reject
//! diagnostic: extensions cannot store
module Conf;

public class Holder
  public o: Object?;

  public constructor()
    o := null;
  end constructor;
end Holder;

public class Box
  public child: Box?;

  public constructor()
    child := null;
  end constructor;

  public method Self(): Box => this;

  public method Child(): Box? => this.child;

  public method Pick(c: bool): Box
    if c then return this; end;
    return this.child!;
  end Pick;
end Box;

public extension Box
  public method M1(h: Holder, c: bool): int
    var a: Box? := null;
    a := this.Self();
    h.o := a;
    return 1;
  end M1;
end extension;
