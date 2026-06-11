public static class Environment
  public static readonly property NewLine: string;
  public static readonly property MachineName: string;
  public static readonly property UserName: string;
  public static readonly property OSVersion: string;
  public static readonly property ProcessorCount: int;
  public static readonly property CommandLineArgs: Array<string>;

  public static method GetEnvironmentVariable(name: string): string?;
  public static method SetEnvironmentVariable(name: string, value: string?);
  public static method GetEnvironmentVariables(): Dictionary<string, string>;

  public static method Exit(exitCode: int);
end Environment;
