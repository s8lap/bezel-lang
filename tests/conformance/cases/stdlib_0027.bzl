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
  public method TryUpgrade(out owned s: Shared<T>?): bool;   // true + a new strong handle
                                                              //   if the value is still alive
  // Does not keep the underlying value alive. Dispose() releases the observer.
end Weak;

using var cacheEntry := new Shared<ParsedConfig>(Parse(text));
_index.Add("primary", cacheEntry.Clone());   // the index owns its own handle
                                             //   (_index: OwnedDictionary<string, Shared<ParsedConfig>>)
Apply(cacheEntry.Get());                     // borrow for the call
