//! case: s11_8_0003
//! doc: spec
//! section: 11.8
//! title: calling a static interface member through the interface name is rejected
//! rule: Static interface members contribute nothing to the instance
//!   surface... calling a static member on the interface name
//!   (INumber<int>.Zero()) is a compile error -- the interface declares no
//!   bodies to run.
//! cites: 11.8
//! expect: reject
//! diagnostic: static
module Conf;

public interface INumber<T> where T: INumber<T>
  static method Zero(): owned T;
end INumber;

public class Meters: INumber<Meters>
  public readonly property Value: double;

  public constructor(value: double)
    Value := value;
  end constructor;

  public static method Zero(): owned Meters => new Meters(0.0);
end Meters;

public class Program
  public static method Test()
    using var m := INumber<Meters>.Zero();
  end Test;
end Program;
