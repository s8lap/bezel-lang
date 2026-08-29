public static class Console
  public static method Write(value: string);   Write(value: Object);
  public static method WriteLine();  WriteLine(value: string);  WriteLine(value: Object);
  // + per-type Write/WriteLine overloads for bool and every numeric primitive
  public static method ReadLine(): string?;            // null at EOF
  public static method ReadKey(): char;                // EndOfStreamException at end of input
  public static method ReadKeyOrNull(): char?;
  public static readonly property In: TextReader;   Out: TextWriter;   Error: TextWriter;
end

public static class Math
  public static const Pi: double;   Tau: double;
  public static pure method Abs(int|long|double|decimal) -> same;
  public static method Min<T>(a: T, b: T): T where T: IComparable<T>;     // NOT pure
  public static method Max<T>(a: T, b: T): T where T: IComparable<T>;     // NOT pure
  public static method Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>;
  public static pure method Sqrt(x: double): double;
  public static pure method Pow(x: double, y: double): double;
  public static method DivRem(a: int, b: int, out remainder: int): int;   // + long form; NOT pure
  public static pure method Floor/Ceiling/Truncate(x: double): double;
  public static pure method Round(x: double[, digits: int]): double;
  public static pure method Floor/Ceiling/Truncate(x: decimal): decimal;  // no Math.Round(decimal)
  public static pure method Sign(int|long|double|decimal): int;
  public static pure method Fmod(x, y: double): double;   IEEERemainder(x, y: double): double;
end
