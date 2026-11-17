module Lib;

public record Pair(tag: int, cents: int): IComparable<Pair>
  public method CompareTo(other: Pair): int
    if cents < other.cents then return -1; end if;
    if cents > other.cents then return 1; end if;
    return 0;
  end CompareTo;
end Pair;

public record G<T>(k: int, n: int, p: T): IComparable<G<T>>
  public method CompareTo(other: G<T>): int
    if n < other.n then return -1; end if;
    if n > other.n then return 1; end if;
    return 0;
  end CompareTo;
end G;
