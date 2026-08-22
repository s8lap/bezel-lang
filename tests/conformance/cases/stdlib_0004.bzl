public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
  // v0.165 (#1357): ordering + construction. CompareTo = tick order (Kind not
  // normalized, matching .NET). Ordering is CompareTo only: DateTime declares no
  // <,>,<=,>= overloads, and records auto-generate equality but never ordering (spec §9.2).
  public constructor(year: int, month: int, day: int);
  public constructor(year: int, month: int, day: int, hour: int, minute: int, second: int,
                     millisecond: int := 0, kind: DateTimeKind := DateTimeKind.Unspecified);
                              // ArgumentOutOfRangeException on invalid components
  public method CompareTo(other: DateTime): int;
  public readonly property Ticks: long;
  public static method FromTicks(ticks: long, kind: DateTimeKind := DateTimeKind.Unspecified): DateTime;
  public method ToUnixTimeSeconds(): long;
  public static method FromUnixTimeSeconds(seconds: long): DateTime;
  public readonly property Year: int;
  public readonly property Month: int;        // 1..12
  public readonly property Day: int;          // 1..31
  public readonly property Hour: int;         // 0..23
  public readonly property Minute: int;       // 0..59
  public readonly property Second: int;       // 0..59
  public readonly property Millisecond: int;
  public readonly property DayOfWeek: DayOfWeek;
  public readonly property DayOfYear: int;
  public readonly property Kind: DateTimeKind;
  public readonly property Date: DateOnly;        // the date components (v0.165)
  public readonly property TimeOfDay: TimeOnly;   // the time-of-day components

  // Now, UtcNow, Today are computed at each access (returning the current
  // date/time); MinValue, MaxValue are constants. All are "readonly" in the
  // property sense — no setter — but only the latter two are constants.
  public static readonly property Now: DateTime;
  public static readonly property UtcNow: DateTime;
  public static readonly property Today: DateTime;
  public static readonly property MinValue: DateTime;
  public static readonly property MaxValue: DateTime;

  public method AddYears(years: int): DateTime;
  public method AddMonths(months: int): DateTime;
  public method AddDays(days: double): DateTime;
  public method AddHours(hours: double): DateTime;
  public method AddMinutes(minutes: double): DateTime;
  public method AddSeconds(seconds: double): DateTime;
  public method AddMilliseconds(ms: double): DateTime;
  public method Add(span: TimeSpan): DateTime;

  public method Subtract(other: DateTime): TimeSpan;
  public method Subtract(span: TimeSpan): DateTime;

  public method ToUtc(): DateTime;
  public method ToLocal(): DateTime;
  public method ToString(): string;                // ISO 8601: "yyyy-MM-ddTHH:mm:ss"
  public method ToString(format: string): string;

  public static method Parse(s: string): DateTime;
  public static method TryParse(s: string, out result: DateTime): bool;
  public static method ParseOrNull(s: string): DateTime?;   // v0.231 (§1 *OrNull tier): null exactly
                                                            //   where Parse(s) raises; otherwise the
                                                            //   parsed value

  public static method operator -(a: DateTime, b: DateTime): TimeSpan;
  public static method operator -(d: DateTime, t: TimeSpan): DateTime;
  public static method operator +(d: DateTime, t: TimeSpan): DateTime;
  public static method operator +(t: TimeSpan, d: DateTime): DateTime;
end DateTime;

public enum DayOfWeek: byte
  Sunday, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday;
end DayOfWeek;

public enum DateTimeKind: byte
  Unspecified, Utc, Local;
end DateTimeKind;

public record TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>
  // v0.165 (#1357): CompareTo = tick order. Ordering is CompareTo only — no
  //   <,>,<=,>= overloads are declared (spec §9.2, §25.2).
  public method CompareTo(other: TimeSpan): int;
  public readonly property Ticks: long;
  public static method FromTicks(ticks: long): TimeSpan;
  public readonly property Days: int;
  public readonly property Hours: int;             // -23..23
  public readonly property Minutes: int;           // -59..59
  public readonly property Seconds: int;           // -59..59
  public readonly property Milliseconds: int;
  public readonly property TotalDays: double;
  public readonly property TotalHours: double;
  public readonly property TotalMinutes: double;
  public readonly property TotalSeconds: double;
  public readonly property TotalMilliseconds: double;

  public static method FromDays(days: double): TimeSpan;
  public static method FromHours(hours: double): TimeSpan;
  public static method FromMinutes(minutes: double): TimeSpan;
  public static method FromSeconds(seconds: double): TimeSpan;
  public static method FromMilliseconds(ms: double): TimeSpan;

  public static readonly property Zero: TimeSpan;          // FromTicks(0) (v0.228)
  public static readonly property MinValue: TimeSpan;      // FromTicks(long.MinValue)
  public static readonly property MaxValue: TimeSpan;      // FromTicks(long.MaxValue)

  public method ToString(): string;                // constant ("c") form (§2.18)
  public method ToString(format: string): string;  // format vocabulary: §2.18

  public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan;
  public static method operator -(a: TimeSpan, b: TimeSpan): TimeSpan;
  public static method operator *(a: TimeSpan, factor: double): TimeSpan;
                                     // computes over Ticks in double; OverflowException on a
                                     //   product outside the tick range or a NaN operand
                                     //   (v0.228 pin)
  public static method operator /(a: TimeSpan, divisor: double): TimeSpan;
                                     // same computation model and OverflowException rule
                                     //   as * (v0.228); DivideByZeroException on
                                     //   divisor = 0.0
  public static method operator -(t: TimeSpan): TimeSpan;
                                     // unary negation (v0.228); OverflowException on MinValue
end TimeSpan;
