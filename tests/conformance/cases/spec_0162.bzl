record TriBool(Value: int)
  public method Negate(): TriBool => case Value
    0 => new TriBool(1),
    1 => new TriBool(0),
    _ => this
  end;
end TriBool;
