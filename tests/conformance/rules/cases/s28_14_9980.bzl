//! case: s28_14_9980
//! doc: spec
//! section: 28.14
//! title: a lambda over a `scoped` formal passed as a function-typed record component is rejected
//! rule: At every function-typed holder the closure bullet's sink rule holds: a store of a function value
//!   admits a capture-free value and rejects one whose source set is non-empty
//! cites: 28.14, 28.3
//! expect: reject
//! diagnostic: may not be stored
//! notes: Pins #9310 (fix c00f5a63f). The record component F is a function-typed holder and the lambda's set
//!   contains the `scoped` formal. Delete the capture (use a capture-free lambda) and the file is
//!   s28_14_9985.
module Conf;
public class FBox
  public f: ((double) -> ())? := null;
  public constructor() end constructor;
end FBox;
public record FRec(F: (double) -> ());
public class Hub
  private owned readonly _keep: FBox := new FBox();
  public method On(scoped handler: (double) -> ())
    var r := new FRec(lambda(t: double) => handler(t));
  end On;
end Hub;
