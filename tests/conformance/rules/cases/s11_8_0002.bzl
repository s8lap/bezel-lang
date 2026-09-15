//! case: s11_8_0002
//! doc: spec
//! section: 11.8
//! title: a static interface member may not declare a body
//! rule: Static interface members are abstract-only (no default bodies; the
//!   same diamond-avoidance posture as the declined default interface
//!   methods).
//! cites: 11.8, 11.2
//! expect: reject
//! diagnostic: body
module Conf;

public interface INumber<T> where T: INumber<T>
  static method Zero(): owned T
  end Zero;
end INumber;
