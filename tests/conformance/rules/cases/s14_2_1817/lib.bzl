module Lib;
public interface IA
  method S<T>(a: Array<T>): int where T: INumber<T>;
  method R<T>(s: string): int where T: INumber<T>;
end IA;
public interface IU
  method R<T>(a: Array<T>): int;
end IU;
public class B
  public virtual method V<T>(a: Array<T>): int where T: INumber<T> => 0;
end B;
public class Dummy
  public static method Z(): int => 0;
end Dummy;
