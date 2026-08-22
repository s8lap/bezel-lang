record TriBool(Value: int)
  public method Negate(): TriBool => (case Value of
    0 => new TriBool(1),
    1 => new TriBool(0),
    _ => this);
end TriBool;
