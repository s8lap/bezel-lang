public extension IEnumerable<T>
  // Filtering
  public method Where(predicate: (T) -> bool): IEnumerable<T>;

  // Projection
  public method Select<U>(selector: (T) -> U): IEnumerable<U>;
  public method SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>;

  // Quantifiers
  public method Any(): bool;
  public method Any(predicate: (T) -> bool): bool;
  public method All(predicate: (T) -> bool): bool;
  public method Count(): int;
  public method Count(predicate: (T) -> bool): int;

  // Element access
  public method First(): T;                                          // throws if empty
  public method First(predicate: (T) -> bool): T;
  public method FirstOrNull(): T?;
  public method FirstOrNull(predicate: (T) -> bool): T?;
  public method Last(): T;
  public method Last(predicate: (T) -> bool): T;
  public method LastOrNull(): T?;
  public method LastOrNull(predicate: (T) -> bool): T?;
  public method Single(): T;                                         // throws if not exactly one
  public method Single(predicate: (T) -> bool): T;                   // throws unless exactly one matches
  public method SingleOrNull(): T?;
  public method SingleOrNull(predicate: (T) -> bool): T?;            // null if none; throws if more than one
  public method ElementAt(index: int): T;
  public method ElementAtOrNull(index: int): T?;      // v0.259 — null when index is out of range

  // Aggregation
  public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
                                               // v0.259 — the §2.19 constrained generic (spec §11.8's
                                               //   accumulator shape); replaces the seven per-type
                                               //   overloads; `owned` erased at value U (spec §28.1)
  public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
  public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
  public method Average(selector: (T) -> int): double;
  public method Average(selector: (T) -> long): double;
  public method Average(selector: (T) -> uint): double;      // v0.195
  public method Average(selector: (T) -> ulong): double;     // v0.195
  public method Average(selector: (T) -> float): float;      // v0.195
  public method Average(selector: (T) -> double): double;
  public method Average(selector: (T) -> decimal): decimal;
  public method Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;

  // Ordering
  public method OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method OrderByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T> where TKey: IComparable<TKey>;
  public method Reverse(): IEnumerable<T>;

  // Slicing
  public method Take(count: int): IEnumerable<T>;
  public method TakeWhile(predicate: (T) -> bool): IEnumerable<T>;
  public method Skip(count: int): IEnumerable<T>;
  public method SkipWhile(predicate: (T) -> bool): IEnumerable<T>;

  // Combination
  public method Concat(other: IEnumerable<T>): IEnumerable<T>;
  public method Zip<U, R>(other: IEnumerable<U>, combine: (T, U) -> R): IEnumerable<R>;

  // Grouping
  public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey: hashable;

  // Materialization (eager)
  public method ToArray(): owned Array<T>;
  public method ToDictionary<TKey>(key: (T) -> TKey): owned Dictionary<TKey, T> where TKey: hashable;
  public method ToDictionary<TKey, TValue>(key: (T) -> TKey, value: (T) -> TValue): owned Dictionary<TKey, TValue> where TKey: hashable;
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
