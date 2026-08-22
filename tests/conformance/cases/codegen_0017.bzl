public interface IComparable<T>    method CompareTo(other: T): int;   end
public interface IEquatable<T>     method Equals(other: T): bool;     end
public interface IDisposable       method Dispose();                  end   // every class implicitly implements
public interface IAsyncDisposable  method DisposeAsync(): Task;       end
public interface IFormattable      method ToString(format: string): string;   end
public class Subscription  destructor; end     // no public ctor; returned as `owned Subscription`
