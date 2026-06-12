public static class Domain
  // No result.
  public static method Spawn(entry: () -> ()): owned Task;
  public static method Spawn<A>(entry: (owned A) -> (), owned arg: A): owned Task
      where A: class;                                      // A sendable (compiler-checked)
  public static method Spawn<A>(entry: (A) -> (), arg: A): owned Task
      where A: notnull;                                    // A sendable value type or string

  // Class-typed result: ownership transfers to the awaiter (Pattern B).
  public static method Spawn<R>(entry: () -> owned R): owned OwnedTask<R>
      where R: class;                                      // R sendable (compiler-checked)
  public static method Spawn<A, R>(entry: (owned A) -> owned R, owned arg: A): owned OwnedTask<R>
      where A: class, R: class;
  public static method Spawn<A, R>(entry: (A) -> owned R, arg: A): owned OwnedTask<R>
      where A: notnull, R: class;

  // Value-typed or string result (Pattern A).
  public static method Spawn<R>(entry: () -> R): owned Task<R>
      where R: notnull;                                    // R sendable value type or string
  public static method Spawn<A, R>(entry: (owned A) -> R, owned arg: A): owned Task<R>
      where A: class, R: notnull;
  public static method Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>
      where A: notnull, R: notnull;
end Domain;
