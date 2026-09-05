record Point(X: int, Y: int): IComparable<Point>
  public method CompareTo(other: Point): int =>
    (if X <> other.X then (if X < other.X then -1 else 1)
     elif Y <> other.Y then (if Y < other.Y then -1 else 1)
     else 0);
end Point;
