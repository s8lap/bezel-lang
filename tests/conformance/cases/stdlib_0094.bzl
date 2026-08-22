public extension IEnumerable<T> where T: IComparable<T>
  public method Min(): T;              // throws if empty
  public method Max(): T;              // throws if empty
  public method MinOrNull(): T?;       // null if empty
  public method MaxOrNull(): T?;       // null if empty
end;
