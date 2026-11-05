//! case: s26_7_1423
//! doc: spec
//! section: 26.7
//! title: an extension cannot store its receiver into a field of the receiver itself
//! rule: The receiver is always a borrow; extensions cannot store `this`, return it as `owned`, or
//!   dispose it.
//! cites: 26.7, 28.3
//! expect: reject
//! diagnostic: extensions cannot store
module Conf;
public class Holder
  public o: Object?;
  public f: (() -> int)?;
  public constructor()
    o := null;
    f := null;
  end constructor;
  public method Take(x: Box): int => 1;
end Holder;
public class Box
  public child: Box?;
  public constructor()
    child := null;
  end constructor;
  public method Self(): Box => this;
  public method Child(): Box? => this.child;
end Box;

public extension Box
  public method M1(h: Holder, c: bool, other: Box): int
    this.child := this;
    return 1;
  end M1;
end extension;
