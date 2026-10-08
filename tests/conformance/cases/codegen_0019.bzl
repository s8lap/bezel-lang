public class Shared<T>: IDisposable where T: class
  public constructor(owned value: T);          // count = 1
  public method Clone(): owned Shared<T>;      // count + 1
  public method Get(): T;                      // borrow; source = this handle
  public method Downgrade(): owned Weak<T>;
end
public class Weak<T>: IDisposable where T: class
  public method TryUpgrade(): owned Shared<T>?;         // Try shape (b)
end
public class ScopeGuard                                  // the sanctioned `errdefer`; NOT YET IMPLEMENTED (#4669)
  public constructor(scoped action: () -> ());           // runs at disposal unless dismissed
  public method Dismiss();                               // idempotent
  public readonly property IsDismissed: bool;
  destructor;
end
public choice Result<T, E>                               // auto-imported
  Ok(owned value: T),
  Error(owned error: E);
  public method IsOk(): bool;      public method IsError(): bool;
  // compiler-provided; admissible ONLY where receiver AND result instantiation are value-like:
  public method Map<U>(f: (T) -> U): Result<U, E>;
  public method MapError<F>(f: (E) -> F): Result<T, F>;
  public method AndThen<U>(f: (T) -> Result<U, E>): Result<U, E>;
  public method UnwrapOr(fallback: T): T;
end
