record Vector2(X: double, Y: double)
  public method Length(): double => Math.Sqrt(X * X + Y * Y);

  public static method operator +(a: Vector2, b: Vector2): Vector2 =>
    new Vector2(a.X + b.X, a.Y + b.Y);
end Vector2;
