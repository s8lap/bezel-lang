public static class Parallel
  // The team's width: the value of Environment.ProcessorCount (§2.4) at team
  // creation, fixed per process. `degree := 0` on the methods below means this value;
  // 1 <= degree caps the workers used by that call; degree < 0 raises ArgumentException.
  public static readonly property DefaultDegree: int;

  // ---- value elements (sendable value types and string): chunks cross as copies ----
  public static method Map<A, R>(items: Array<A>, body: (A) -> R,
      degree: int := 0): owned Task<Array<R>>
      where A: notnull, R: notnull;              // Pattern A result: the task owns the
                                                 //   array, awaiters borrow (the §6.7 WhenAll<T> shape)

  public static method Reduce<A, R>(items: Array<A>, seed: R,
      selector: (A) -> R,                        // spawn position
      combiner: (R, R) -> R,                     // spawn position; MUST be associative
      degree: int := 0): owned Task<R>
      where A: notnull, R: notnull;

  public static method ForEach<A>(items: Array<A>, body: (A) -> (),
      degree: int := 0): owned Task
      where A: notnull;

  // ---- class elements: ownership partitions in, results partition back ----
  public static method Map<A, R>(owned items: OwningArray<A>, body: (owned A) -> owned R,
      degree: int := 0): owned OwningTask<OwningArray<R>>
      where A: class, R: class;                  // A, R sendable (compiler-checked)

  public static method ForEach<A>(owned items: OwningArray<A>, body: (owned A) -> (),
      degree: int := 0): owned Task
      where A: class;

  // ---- frozen elements (§4.19): zero-copy — one Shared handle cloned per worker ----
  public static method Map<A, R>(items: Shared<FrozenArray<A>>, body: (A) -> R,
      degree: int := 0): owned Task<Array<R>>
      where A: notnull, R: notnull;
  public static method Reduce<A, R>(items: Shared<FrozenArray<A>>, seed: R,
      selector: (A) -> R, combiner: (R, R) -> R,
      degree: int := 0): owned Task<R>
      where A: notnull, R: notnull;
  public static method ForEach<A>(items: Shared<FrozenArray<A>>, body: (A) -> (),
      degree: int := 0): owned Task
      where A: notnull;
end Parallel;
