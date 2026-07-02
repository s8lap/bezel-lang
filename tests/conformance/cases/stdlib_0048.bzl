public class Set<T>: ICollection<T>
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);

  // ICollection<T> operations (including Add(item: T) returning ()) plus:
  public method TryAdd(item: T): bool;                 // returns whether added
                                                       //   (false if duplicate)
  public method UnionWith(other: IEnumerable<T>);
  public method IntersectWith(other: IEnumerable<T>);
  public method ExceptWith(other: IEnumerable<T>);
  public method IsSubsetOf(other: IEnumerable<T>): bool;
  public method IsSupersetOf(other: IEnumerable<T>): bool;
end Set;

public class OwningSet<T>: IOwningCollection<T> where T: class
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);

  // IOwningCollection<T> operations plus:
  public method TryAdd(owned item: T): bool;          // returns whether added
                                                       //   (false if duplicate;
                                                       //   passed item is disposed)
  // Drain
  public method Take(query: T): owned T;               // borrow-taking query;
                                                       //   returns owned matched
  public method TryTake(query: T, out owned removed: T?): bool;
  public method Drain(): IOwningEnumerable<T>;

  public method UnionWith(owned other: OwningSet<T>);   // drains other into self;
                                                       //   duplicates disposed
  public method IntersectWith(other: IEnumerable<T>);  // disposes elements not in other
  public method ExceptWith(other: IEnumerable<T>);     // disposes elements in other
  public method IsSubsetOf(other: IEnumerable<T>): bool;
  public method IsSupersetOf(other: IEnumerable<T>): bool;
end OwningSet;
