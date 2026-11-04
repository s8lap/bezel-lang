//! case: s9_8_0045
//! doc: spec
//! section: 9.8
//! title: a borrow-bearing choice declaring a serialization interface is rejected
//! rule: A choice is **serialization-eligible** when it is value-like (§35.4)
//!   and every payload field's type is admissible as a serialized component and
//!   record-admissible (value-shaped storage throughout; the borrow-bearing and
//!   owning classifications are never eligible — the same
//!   `Deserialize`-has-no-owner argument as for class-typed record fields). An
//!   ineligible conformance declaration is the compile error above, naming the
//!   first offending payload.
//! cites: 9.8, 35.4
//! expect: reject
//! diagnostic: value-like
//! notes: twin s9_8_0050 (next free id; AUTHORING.md asks for adjacent ids, and renumbering
//!   the batch would churn every cross-reference).
module Conf;

public class Cell
  public constructor()
  end constructor;
end Cell;

public choice Slot: ISerializable, IDeserializable
  Held(c: Cell),
  Free;
end Slot;
