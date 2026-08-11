public interface INumber<T> where T: INumber<T>
  static method Zero(): owned T;               // producers spell owned (v0.196): a fresh value at
  static method One(): owned T;                //   class/interface conformers; erased at value ones (§28.1)
  static operator +(a: T, b: T): owned T;
  static operator -(a: T, b: T): owned T;
  static operator *(a: T, b: T): owned T;
  static operator /(a: T, b: T): owned T;      // raises per the operand type's division rules
  static operator <(a: T, b: T): bool;
end INumber;

public interface ISteppable<T> where T: ISteppable<T>
  static method Step(value: T, by: long): owned T;   // advance in T's space — a fresh value
  static operator <(a: T, b: T): bool;
end ISteppable;

public interface IParsable<T> where T: IParsable<T>
  static method Parse(s: string): owned T;                       // FormatException on bad input
  static method TryParse(s: string, out owned value: T?): bool;  // the §1 Try convention
end IParsable;
