public class SortedSet<T>: IDisposable where T: IComparable<T>
  public constructor();

  public readonly property Count: int;

  public method Add(item: T): bool;             // false if an equal item is present
  public method Contains(item: T): bool;
  public method Remove(item: T): bool;          // false if absent
  public method Clear();

  public readonly property First: T;            // least; InvalidOperationException if empty
  public readonly property Last: T;             // greatest; InvalidOperationException if empty
end SortedSet;
