/// Compiler-intrinsic; T restricted to primitive integer types.
public class AtomicInteger<T> where T: value
  public constructor(initial: T);

  public property Value: T
    get;
    set;
  end Value;

  public method Exchange(value: T): T;
  public method CompareExchange(expected: T, replacement: T): T;

  public method Increment(): T;        // returns post-increment value
  public method Decrement(): T;        // returns post-decrement value
  public method Add(delta: T): T;      // returns post-add value
end AtomicInteger;
