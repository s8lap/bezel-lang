//! case: s28_3_0110
//! doc: spec
//! section: 28.3
//! title: a function value read from a property carries the empty set and may be stored in a field
//! rule: A function value **read from a field, a collection, a property or an
//!   indexer**, or **narrowed from `Object`**, or from a value whose type is a
//!   bare type parameter, by an `as` or a typed-pattern binding (§17.4,
//!   §19.3), carries the empty set: those holders, every position declared
//!   `Object`, and every position a generic body declares at a type parameter
//!   (*Set-erasing positions* below), admit only capture-free function values
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
