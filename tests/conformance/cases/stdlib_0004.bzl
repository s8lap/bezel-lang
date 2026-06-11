public record DateTime
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
  public method ToString(format: string): string;

  public static method Parse(s: string): DateTime;
  public static method TryParse(s: string, out result: DateTime): bool;

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

public record TimeSpan
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

  public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan;
  public static method operator -(a: TimeSpan, b: TimeSpan): TimeSpan;
  public static method operator *(a: TimeSpan, factor: double): TimeSpan;
end TimeSpan;
