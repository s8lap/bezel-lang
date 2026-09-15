//! case: s11_8_0001
//! doc: spec
//! section: 11.8
//! title: a conforming type supplies static interface members with exact signatures
//! rule: An interface member may be declared static: a contract the
//!   conforming type -- not an instance -- must satisfy. A conforming type
//!   supplies the static members with exact signatures.
//! cites: 11.8
//! expect: accept
module Conf;

public interface INumber<T> where T: INumber<T>
  static method Zero(): owned T;
  static operator +(a: T, b: T): owned T;
end INumber;

public class Meters: INumber<Meters>
  public readonly property Value: double;

  public constructor(value: double)
    Value := value;
  end constructor;

  public static method Zero(): owned Meters => new Meters(0.0);
  public static method operator +(a: Meters, b: Meters): owned Meters => new Meters(a.Value + b.Value);
end Meters;
