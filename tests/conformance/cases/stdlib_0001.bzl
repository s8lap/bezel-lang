public static class Math
  public static const Pi: double := 3.14159265358979323846;
  public static const E: double := 2.71828182845904523536;
  public static const Tau: double := 6.28318530717958647692;

  public static method Abs(x: int): int;
  public static method Abs(x: long): long;
  public static method Abs(x: double): double;
  public static method Abs(x: decimal): decimal;

  public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
  public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
  public static method Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>;

  public static method Sqrt(x: double): double;
  public static method Pow(x: double, y: double): double;
  public static method Exp(x: double): double;
  public static method Log(x: double): double;       // natural log
  public static method Log(x: double, base_: double): double;
  public static method Log10(x: double): double;
  public static method Log2(x: double): double;

  public static method Sin(x: double): double;
  public static method Cos(x: double): double;
  public static method Tan(x: double): double;
  public static method Asin(x: double): double;
  public static method Acos(x: double): double;
  public static method Atan(x: double): double;
  public static method Atan2(y: double, x: double): double;
  public static method Sinh(x: double): double;
  public static method Cosh(x: double): double;
  public static method Tanh(x: double): double;

  public static method Floor(x: double): double;
  public static method Ceiling(x: double): double;
  public static method Round(x: double): double;     // banker's rounding
  public static method Round(x: double, digits: int): double;
  public static method Truncate(x: double): double;

  public static method Sign(x: int): int;            // -1, 0, or 1
  public static method Sign(x: long): int;
  public static method Sign(x: double): int;

  public static method Fmod(x: double, y: double): double;
  public static method IEEERemainder(x: double, y: double): double;
end Math;
