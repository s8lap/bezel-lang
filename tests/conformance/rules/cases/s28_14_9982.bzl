//! case: s28_14_9982
//! doc: spec
//! section: 28.14
//! title: a lambda over a `scoped` formal as a `??` right operand stored into a field is rejected
//! rule: It may not return it, move it into non-`scoped` storage, or store it in any other object.
//! cites: 28.14, 28.3, K7
//! expect: reject
//! diagnostic: may not be stored
//! notes: Pins #9310 (fix c00f5a63f): the `??` meet carries the union of its operands' source sets (K7), so
//!   the stored value carries `handler`.
module Conf;
public class FBox
  public f: ((double) -> ())? := null;
  public constructor() end constructor;
end FBox;
public record FRec(F: (double) -> ());
public class Hub
  private owned readonly _keep: FBox := new FBox();
  public method On(scoped handler: (double) -> ())
    var g: ((double) -> ())? := null;
    _keep.f := g ?? lambda(t: double) => handler(t);
  end On;
end Hub;
