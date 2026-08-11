/// Compiler-intrinsic; T restricted to cell-sized primitive types.
public class Atomic<T> where T: value
  public constructor(initial: T);

  public property Value: T
    get;       // hardware-atomic read
    set;       // hardware-atomic write
  end Value;

  public method Exchange(value: T): T;
  public method CompareExchange(expected: T, replacement: T): T;
end Atomic;
