public extension IEnumerable<T>
  // Filtering
  public method Where(predicate: (T) -> bool): IEnumerable<T>;
  public method OfType<U>(): IEnumerable<U>;

  // Projection
  public method Select<U>(selector: (T) -> U): IEnumerable<U>;
  public method SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>;

  // Quantifiers
  public method Any(): bool;
  public method Any(predicate: (T) -> bool): bool;
  public method All(predicate: (T) -> bool): bool;
  public method Count(): int;
  public method Count(predicate: (T) -> bool): int;
  public method Contains(value: T): bool;

  // Element access
  public method First(): T;                                          // throws if empty
  public method First(predicate: (T) -> bool): T;
  public method FirstOrNull(): T?;
  public method FirstOrNull(predicate: (T) -> bool): T?;
  public method Last(): T;
  public method LastOrNull(): T?;
  public method Single(): T;                                         // throws if not exactly one
  public method SingleOrNull(): T?;
  public method ElementAt(index: int): T;

  // Aggregation
  public method Sum(selector: (T) -> int): int;
  public method Sum(selector: (T) -> long): long;
  public method Sum(selector: (T) -> uint): uint;
  public method Sum(selector: (T) -> ulong): ulong;
  public method Sum(selector: (T) -> float): float;
  public method Sum(selector: (T) -> double): double;
  public method Sum(selector: (T) -> decimal): decimal;
  public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
  public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
  public method Average(selector: (T) -> int): double;
  public method Average(selector: (T) -> long): double;
  public method Average(selector: (T) -> double): double;
  public method Average(selector: (T) -> decimal): decimal;
  public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;

  // Ordering
  public method OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method OrderByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method Reverse(): IEnumerable<T>;

  // Set-like
  public method Distinct(): IEnumerable<T>;
  public method Union(other: IEnumerable<T>): IEnumerable<T>;
  public method Intersect(other: IEnumerable<T>): IEnumerable<T>;
  public method Except(other: IEnumerable<T>): IEnumerable<T>;

  // Slicing
  public method Take(count: int): IEnumerable<T>;
  public method TakeWhile(predicate: (T) -> bool): IEnumerable<T>;
  public method Skip(count: int): IEnumerable<T>;
  public method SkipWhile(predicate: (T) -> bool): IEnumerable<T>;

  // Combination
  public method Concat(other: IEnumerable<T>): IEnumerable<T>;
  public method Zip<U, R>(other: IEnumerable<U>, combine: (T, U) -> R): IEnumerable<R>;

  // Grouping
  public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>>;

  // Materialization (eager)
  public method ToArray(): owned Array<T>;
  public method ToDictionary<TKey>(key: (T) -> TKey): owned Dictionary<TKey, T>;
  public method ToDictionary<TKey, TValue>(key: (T) -> TKey, value: (T) -> TValue): owned Dictionary<TKey, TValue>;
  public method ToSet(): owned Set<T>;
end;

public interface IGrouping<TKey, T>: IEnumerable<T>
  readonly property Key: TKey;
end IGrouping;

public interface IOrderedEnumerable<T>: IEnumerable<T>
end IOrderedEnumerable;

public extension IOrderedEnumerable<T>
  public method ThenBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method ThenByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
end;
