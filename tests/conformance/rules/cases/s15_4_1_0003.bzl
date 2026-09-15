//! case: s15_4_1_0003
//! doc: spec
//! section: 15.4.1
//! title: a record may not conform to a sequence-literal target interface
//! rule: A value kind — record, choice, or enum — may not conform to either
//!   sequence-literal target interface: the literal's result is a fresh owned
//!   heap value, and the §11.8 erasure of the member's `owned` return at a
//!   value kind does not make one a target (a compile error at the
//!   declaration) (§15.4.1).
//! cites: 15.4.1, 11.8
//! expect: reject
//! diagnostic: conform
module Conf;

public record Trio(A: int, B: int, C: int): ICollectionLiteralTarget<Trio, int>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<int>): Trio
    return new Trio(elements[0], elements[1], elements[2]);
  end FromCollectionLiteral;
end Trio;
