public class Set<T>: ICollection<T>, ICollectionLiteralTarget<Set<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Set<T>;   // §2.19 witness; duplicates
                                                                                              //   collapse (spec §15.4)
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);
  public constructor(capacity: int, comparer: IEqualityComparer<T>);   // v0.195: the §4.8 combined form

  // ICollection<T> operations (including Add(item: T) returning ()) plus:
  public method TryAdd(item: T): bool;                 // returns whether added
                                                       //   (false if duplicate)
  public method UnionWith(other: IEnumerable<T>);
  public method IntersectWith(other: IEnumerable<T>);
  public method ExceptWith(other: IEnumerable<T>);
  public method IsSubsetOf(other: IEnumerable<T>): bool;
  public method IsSupersetOf(other: IEnumerable<T>): bool;
end Set;

public class OwningSet<T>: IOwningCollection<T>, IOwningCollectionLiteralTarget<OwningSet<T>, T> where T: class
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningSet<T>;   // §2.19 witness
  public constructor();
  public constructor(capacity: int);
  public constructor(comparer: IEqualityComparer<T>);
  public constructor(capacity: int, comparer: IEqualityComparer<T>);   // v0.195

  // IOwningCollection<T> operations plus:
  public method TryAdd(owned item: T): bool;          // returns whether added
                                                       //   (false if duplicate;
                                                       //   passed item is disposed)
  // Drain
  public method Pop(query: T): owned T;                // borrow-taking query;
                                                       //   returns owned matched
                                                       //   (v0.259: renamed from
                                                       //   Take/TryTake — below)
  public method TryPop(query: T, out owned removed: T?): bool;
  public method Drain(): IOwningEnumerable<T>;

  public method UnionWith(owned other: OwningSet<T>);   // drains other into self;
                                                       //   duplicates disposed
  public method IntersectWith(other: IEnumerable<T>);  // disposes elements not in other
  public method ExceptWith(other: IEnumerable<T>);     // disposes elements in other
  public method IsSubsetOf(other: IEnumerable<T>): bool;
  public method IsSupersetOf(other: IEnumerable<T>): bool;
end OwningSet;
