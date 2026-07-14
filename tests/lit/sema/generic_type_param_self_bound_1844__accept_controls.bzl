// RUN: %bezelc -c -o %t.o %s
//
// spec §14.2, v0.196 / #1844 (over-reject guard): the declaration-time self /
// cycle check must reject ONLY a type-parameter-to-type-parameter self or cycle.
// These well-formed declarations must keep compiling:
//   * `where T: U` — a plain, non-cyclic type-parameter bound.
//   * `where T: IComparable<T>` — `T` appears only as a generic ARGUMENT; the
//     bound's head is the interface `IComparable`, so this is NOT a self-bound.
//   * `where T: U where U: V` — a non-cyclic chain across three parameters.
// A regression that over-rejected any of these would flip this compile to nonzero.

module M;

public class Pair<T, U> where T: U
  public V: T?;
end Pair;

public class Sorter<T> where T: IComparable<T>
  public V: T?;
end Sorter;

public class Chain<T, U, V> where T: U where U: V
  public X: T?;
end Chain;
