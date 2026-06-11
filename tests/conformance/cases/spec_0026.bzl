enum Priority: IComparable<Priority>
  Low, Medium, High;

  public method CompareTo(other: Priority): int =>
    case
      int(this) < int(other) => -1,
      int(this) > int(other) => 1,
      _ => 0
    end;
end Priority;
