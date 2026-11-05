//! case: s26_7_1409
//! doc: spec
//! section: 26.7
//! title: an extension over IEnumerable<int> cannot store its receiver
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

public extension IEnumerable<int>
  public method M1(h: Holder): int
    h.o := this;
    return 1;
  end M1;
end extension;
