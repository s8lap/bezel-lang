public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>   // conformance added v0.165 (#1354); explicit IDisposable dropped v0.195
  public constructor();

  public pure readonly property Count: int;

  public method Add(item: T);                   // v0.165 (#1354): unit-returning, matching §4.10's convention
  public method TryAdd(item: T): bool;          // false if an equal item is present
  public method Contains(item: T): bool;
  public method Remove(item: T): bool;          // false if absent
  public method Clear();

  public pure readonly property First: T;            // least; InvalidOperationException if empty
  public pure readonly property Last: T;             // greatest; InvalidOperationException if empty
end SortedSet;
