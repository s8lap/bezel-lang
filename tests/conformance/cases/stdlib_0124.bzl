public static class Domain
  // No result.
  public static method Spawn(entry: () -> ()): owned Task;
  public static method Spawn<A>(entry: (owned A) -> (), owned arg: A): owned Task
      where A: sendable, class;
  public static method Spawn<A>(entry: (A) -> (), arg: A): owned Task
      where A: sendable, notnull;

  // Class-typed result: ownership transfers to the awaiter (Pattern B).
  public static method Spawn<R>(entry: () -> owned R): owned OwningTask<R>
      where R: sendable, class;
  public static method Spawn<A, R>(entry: (owned A) -> owned R, owned arg: A): owned OwningTask<R>
      where A: sendable, class where R: sendable, class;
  public static method Spawn<A, R>(entry: (A) -> owned R, arg: A): owned OwningTask<R>
      where A: sendable, notnull where R: sendable, class;

  // Value-typed or string result (Pattern A).
  public static method Spawn<R>(entry: () -> R): owned Task<R>
      where R: sendable, notnull;
  public static method Spawn<A, R>(entry: (owned A) -> R, owned arg: A): owned Task<R>
      where A: sendable, class where R: sendable, notnull;
  public static method Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>
      where A: sendable, notnull where R: sendable, notnull;

  // Async entries (v0.179; spec §30.14 "Async entries"): the runtime awaits the
  // returned task inside the spawned domain; the join handle tracks that task.
  public static method Spawn(entry: () -> Task): owned Task;
  public static method Spawn<A>(entry: (owned A) -> Task, owned arg: A): owned Task
      where A: sendable, class;
  public static method Spawn<A>(entry: (A) -> Task, arg: A): owned Task
      where A: sendable, notnull;

  public static method Spawn<R>(entry: () -> OwningTask<R>): owned OwningTask<R>
      where R: sendable, class;
  public static method Spawn<A, R>(entry: (owned A) -> OwningTask<R>, owned arg: A): owned OwningTask<R>
      where A: sendable, class where R: sendable, class;
  public static method Spawn<A, R>(entry: (A) -> OwningTask<R>, arg: A): owned OwningTask<R>
      where A: sendable, notnull where R: sendable, class;

  public static method Spawn<R>(entry: () -> Task<R>): owned Task<R>
      where R: sendable, notnull;
  public static method Spawn<A, R>(entry: (owned A) -> Task<R>, owned arg: A): owned Task<R>
      where A: sendable, class where R: sendable, notnull;
  public static method Spawn<A, R>(entry: (A) -> Task<R>, arg: A): owned Task<R>
      where A: sendable, notnull where R: sendable, notnull;
end Domain;
