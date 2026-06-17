record Point(X: int, Y: int): IComparable<Point>
  public method CompareTo(other: Point): int =>
    when
      X <> other.X => when
        X < other.X => -1,
        _ => 1
      end,
      Y <> other.Y => when
        Y < other.Y => -1,
        _ => 1
      end,
      _ => 0
    end;
end Point;
