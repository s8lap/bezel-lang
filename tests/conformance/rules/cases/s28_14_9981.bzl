//! case: s28_14_9981
//! doc: spec
//! section: 28.14
//! title: a lambda over a `scoped` formal placed in a local array literal is rejected
//! rule: While its source set is non-empty, a function value may not be stored in a field, in any collection,
//!   in a property or indexer, or through an `out` or `ref` parameter
//! cites: 28.14, 28.3
//! expect: reject
//! diagnostic: may not be stored
//! notes: Pins #9310 (fix c00f5a63f): a collection slot is a function-typed holder even for a local
//!   collection.
module Conf;
public class FBox
  public f: ((double) -> ())? := null;
  public constructor() end constructor;
end FBox;
public record FRec(F: (double) -> ());
public class Hub
  private owned readonly _keep: FBox := new FBox();
  public method On(scoped handler: (double) -> ())
    var arr: Array<(double) -> ()> := [lambda(t: double) => handler(t)];
  end On;
end Hub;
