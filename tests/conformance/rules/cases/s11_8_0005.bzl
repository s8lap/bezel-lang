//! case: s11_8_0005
//! doc: spec
//! section: 11.8
//! title: T.Member(...) dispatches to the conforming type's static member
//! rule: In a generic body whose parameter T carries a static-interface
//!   constraint, T.Member(...) calls the conforming type's static member,
//!   and an operator applied to T-typed operands resolves through the
//!   constraint's static operators.
//! cites: 11.8
//! expect: run
//! stdout: 2.5
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

public static class Calc
  public static method Describe<T>(seed: T): owned T where T: INumber<T>
    using var zero := T.Zero();
    return zero + seed;
  end Describe;
end Calc;

public class Program
  public static method Main()
    using var seed := new Meters(2.5);
    using var total := Calc.Describe(seed);
    Console.WriteLine(total.Value);
  end Main;
end Program;
