//! case: s28_3_0110
//! doc: spec
//! section: 28.3
//! title: a function value read from a property carries the empty set and may be stored in a field
//! rule: A function value read from a field, a collection, a property or an
//!   indexer carries the empty set: those holders admit only capture-free
//!   function values
//! cites: 28.3
//! expect: accept
module Conf;

public class Source
  public property Cb: () -> int;

  public constructor()
    Cb := lambda() => 1;
  end constructor;
end Source;

public class Sink
  private _g: () -> int;

  public constructor()
    _g := lambda() => 0;
  end constructor;

  public method Take(s: Source): int
    _g := s.Cb;
    var h := _g;
    return h();
  end Take;
end Sink;
