enum Priority: IComparable<Priority>
  Low, Medium, High;

  public method CompareTo(other: Priority): int =>
    (if int(this) < int(other) then -1
     elif int(this) > int(other) then 1
     else 0);
end Priority;
