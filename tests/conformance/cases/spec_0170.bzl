record Vector2(X: double, Y: double)
  public static method operator +(left: Vector2, right: Vector2): Vector2 =>
    new Vector2(left.X + right.X, left.Y + right.Y);

  public static method operator *(v: Vector2, scalar: double): Vector2 =>
    new Vector2(v.X * scalar, v.Y * scalar);
end Vector2;
