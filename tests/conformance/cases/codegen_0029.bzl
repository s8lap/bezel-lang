public extension IEnumerable<T>
  // Filtering / projection (LAZY)
  Where(predicate: (T) -> bool): IEnumerable<T>;
  Select<U>(selector: (T) -> U): IEnumerable<U>;
  SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>;
  // Quantifiers and element access (EAGER)
  Any(): bool;   Any(predicate): bool;   All(predicate): bool;
  Count(): int;  Count(predicate): int;
  First(): T;  First(predicate): T;   FirstOrNull(): T?;  FirstOrNull(predicate): T?;
  Last(): T;   Last(predicate): T;    LastOrNull(): T?;   LastOrNull(predicate): T?;
  Single(): T; Single(predicate): T;  SingleOrNull(): T?; SingleOrNull(predicate): T?;
  ElementAt(index: int): T;           ElementAtOrNull(index: int): T?;       // NO predicate form
  // Aggregation (EAGER)
  Sum<U>(selector: (T) -> U): owned U            where U: INumber<U>;
  Min<U>(selector: (T) -> U): U                  where U: IComparable<U>;
  Max<U>(selector: (T) -> U): U                  where U: IComparable<U>;
  Average(selector: (T) -> int|long|uint|ulong|double): double;
  Average(selector: (T) -> float): float;        Average(selector: (T) -> decimal): decimal;
  Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;
  // Ordering (LAZY; the OrderBy family is STABLE)
  OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>            where TKey: IComparable<TKey>;
  OrderByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>  where TKey: IComparable<TKey>;
  Reverse(): IEnumerable<T>;
  // (the hashable-requiring operators are a separate constrained block, below)
  // Slicing (LAZY; counts CLAMP)
  Take(count: int); TakeWhile(predicate); Skip(count: int); SkipWhile(predicate): IEnumerable<T>;
  // Combination and grouping (LAZY)
  Concat(other: IEnumerable<T>): IEnumerable<T>;
  Zip<U, R>(other: IEnumerable<U>, combine: (T, U) -> R): IEnumerable<R>;   // ends when either ends
  GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>>   where TKey: hashable;
  // Materialization (EAGER)
  ToArray(): owned Array<T>;
  ToDictionary<TKey>(key: (T) -> TKey): owned Dictionary<TKey, T>         where TKey: hashable;
  ToDictionary<TKey, TValue>(key: (T) -> TKey, value: (T) -> TValue):
      owned Dictionary<TKey, TValue>                                       where TKey: hashable;
end
// The six hashable-element operators are a SECOND extension block (v0.277). §26.6 merges
// the two, so call sites are unchanged -- but write `where T: hashable` if you declare
// your own, and expect a non-hashable T to be rejected here rather than inside Set<T>.
public extension IEnumerable<T> where T: hashable
  Contains(value: T): bool;
  Distinct(): IEnumerable<T>;
  Union(other: IEnumerable<T>): IEnumerable<T>;
  Intersect(other: IEnumerable<T>): IEnumerable<T>;
  Except(other: IEnumerable<T>): IEnumerable<T>;
  ToSet(): owned Set<T>;
end
public interface IGrouping<TKey, T>: IEnumerable<T>   readonly property Key: TKey;   end
public interface IOrderedEnumerable<T>: IEnumerable<T>  end
public extension IOrderedEnumerable<T>
  ThenBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>            where TKey: IComparable<TKey>;
  ThenByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>  where TKey: IComparable<TKey>;
end
public extension IEnumerable<T> where T: IComparable<T>   Min(): T; Max(): T; MinOrNull(): T?; MaxOrNull(): T?;  end
public extension IEnumerable<T> where T: INumber<T>       Sum(): owned T;  end
public extension IEnumerable<int|long|uint|ulong|double>  Average(): double;  end
public extension IEnumerable<float>    Average(): float;    end
public extension IEnumerable<decimal>  Average(): decimal;  end
