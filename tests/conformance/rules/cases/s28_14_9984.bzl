//! case: s28_14_9984
//! doc: spec
//! section: 28.14
//! title: a lambda over a `scoped` formal returned by an invoked maker and stored into a field is rejected
//! rule: It may not return it, move it into non-`scoped` storage, or store it in any other object.
//! cites: 28.14, 28.3
//! expect: reject
//! diagnostic: may not be stored
//! notes: Pins #9310 (fix c00f5a63f): invoking a function value yields a value carrying the invoked value's
//!   set (provenance clause 4), so `mk()` carries `handler`.
module Conf;
public class FBox
  public f: ((double) -> ())? := null;
  public constructor() end constructor;
end FBox;
public record FRec(F: (double) -> ());
public class Hub
  private owned readonly _keep: FBox := new FBox();
  public method On(scoped handler: (double) -> ())
    var mk := lambda() => lambda(t: double) => handler(t);
    _keep.f := mk();
  end On;
end Hub;
