public static class Math
  public static const Pi: double := 3.14159265358979323846;
  public static const Tau: double := 6.28318530717958647692;

  public static pure method Abs(x: int): int;
  public static pure method Abs(x: long): long;
  public static pure method Abs(x: double): double;
  public static pure method Abs(x: decimal): decimal;

  public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
  public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
  public static method Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>;
      // low > high: ArgumentException (v0.195)

  public static pure method Sqrt(x: double): double;
  public static pure method Pow(x: double, y: double): double;
  public static method DivRem(a: int, b: int, out remainder: int): int;    // v0.215: returns the quotient;
  public static method DivRem(a: long, b: long, out remainder: long): long; //   b = 0 raises DivideByZeroException
                                                                            //   (§6.1, as `/`). Not `pure`: a pure
                                                                            //   member declares no `out` (§22.15);
                                                                            //   the tuple shape 0062 sketched does
                                                                            //   not exist (no tuples).


  public static pure method Floor(x: double): double;
  public static pure method Ceiling(x: double): double;
  public static pure method Round(x: double): double;     // banker's rounding
  public static pure method Round(x: double, digits: int): double;
  public static pure method Truncate(x: double): double;

  public static pure method Floor(x: decimal): decimal;    // toward negative infinity (v0.228);
  public static pure method Ceiling(x: decimal): decimal;  //   toward positive infinity — each to an
  public static pure method Truncate(x: decimal): decimal; //   integral value; exact, never faults
                                                           //   (decimal's range endpoints are integral).
                                                           //   Truncate is toward zero. Round(decimal)
                                                           //   is the instance Decimal.Round (§9.3).

  public static pure method Sign(x: int): int;            // -1, 0, or 1
  public static pure method Sign(x: long): int;
  public static pure method Sign(x: double): int;
  public static pure method Sign(x: decimal): int;        // -1, 0, or 1 (v0.228)

  public static pure method Fmod(x: double, y: double): double;
  public static pure method IEEERemainder(x: double, y: double): double;
end Math;
