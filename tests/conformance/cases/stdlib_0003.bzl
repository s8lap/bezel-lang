public static class Environment
  public static readonly property NewLine: string;
  public static readonly property MachineName: string;
  public static readonly property UserName: string;
  public static readonly property OSVersion: string;
  public static readonly property ProcessorCount: int;
  public static readonly property CommandLineArgs: IReadOnlyList<string>;   // read-only view of cached process-lived storage (§4.1); dedicated view object, not an Array — §2.13's downcast rule (#1805)

  public static method Get(name: string): string?;    // null when the variable is absent (v0.231)
  public static method GetInt(name: string): int?;    // v0.215 convenience tier: null when the variable is
  public static method GetBool(name: string): bool?;  //   absent OR malformed — deliberately conflated
                                                      //   (recorded): callers that must distinguish keep the
                                                      //   two-step idiom (Get + TryParse).
                                                      //   GetBool accepts exactly Boolean.Parse's forms
                                                      //   ("true"/"false", case-insensitive); "1"/"yes" are null.
  public static method Set(name: string, value: string?);
      // value = null removes the variable; an empty name, or one containing "=",
      //   raises ArgumentException (v0.195)
  public static method Variables(): owned Dictionary<string, string>;

  public static method Exit(exitCode: int);
end Environment;
