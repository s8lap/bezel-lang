public class Shared<T>: IDisposable where T: class      // proper class types only;
                                                         //   runtime-managed T rejected at instantiation
  public constructor(owned value: T);                    // consumes the value; count = 1

  public method Clone(): owned Shared<T>;                // count + 1; a new independent handle
  public method Get(): T;                                // borrow of the underlying value;
                                                         //   source: this handle (valid while it lives)
  public method Downgrade(): owned Weak<T>;              // mint a non-owning observer

  // Dispose(): count - 1; the last disposal disposes the underlying value.
end Shared;

public class Weak<T>: IDisposable where T: class
  public method TryUpgrade(): owned Shared<T>?;              // a new strong handle if the value
                                                              //   is still alive; null otherwise
                                                              //   (§1 Try shape (b))
  // Does not keep the underlying value alive. Dispose() releases the observer.
end Weak;
