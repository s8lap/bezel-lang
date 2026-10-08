public static class Convert     // string <-> T only; there is no Convert.ToInt32(double)
  ToInt32/ToInt64/ToDouble/ToDecimal/ToBoolean(value: string);
  TryToInt32/.../TryToBoolean(value: string, out result: T): bool;         // shape (c)
  ToString(int|long|double|decimal|bool);   ToString(value: int|long, toBase: int);  // bases 2/8/10/16
end                                                    // faults: FormatException, OverflowException

public static class Environment
  public static readonly property NewLine, MachineName, UserName, OSVersion: string;   // UserName NOT YET IMPLEMENTED (#5579)
  public static readonly property ProcessorCount: int;
  public static readonly property CommandLineArgs: IReadOnlyList<string>;   // NOT YET IMPLEMENTED (#5579)
  public static method Get(name: string): string?;
  public static method GetInt(name: string): int?;   GetBool(name: string): bool?;
  public static method Set(name: string, value: string?);      // null removes
  public static method Variables(): owned Dictionary<string, string>;
  public static method Exit(exitCode: int);
end
