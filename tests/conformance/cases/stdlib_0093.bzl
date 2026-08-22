public extension IEnumerable<T> where T: hashable
  // Quantifiers
  public method Contains(value: T): bool;

  // Set-like
  public method Distinct(): IEnumerable<T>;
  public method Union(other: IEnumerable<T>): IEnumerable<T>;
  public method Intersect(other: IEnumerable<T>): IEnumerable<T>;
  public method Except(other: IEnumerable<T>): IEnumerable<T>;

  // Materialization (eager)
  public method ToSet(): owned Set<T>;
end;
