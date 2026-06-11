public static class File
  public static method ReadAllText(path: string): string;
  public static method WriteAllText(path: string, contents: string);
  public static method ReadAllBytes(path: string): owned Array<byte>;
  public static method WriteAllBytes(path: string, contents: Array<byte>);
  public static method ReadAllLines(path: string): owned Array<string>;
  public static method WriteAllLines(path: string, lines: IEnumerable<string>);

  public static async method ReadAllTextAsync(path: string,
                                                token: CancellationToken := CancellationToken.None): Task<string>;
  public static async method WriteAllTextAsync(path: string, contents: string,
                                                 token: CancellationToken := CancellationToken.None): Task;

  public static method Exists(path: string): bool;
  public static method Delete(path: string);
  public static method Copy(source: string, destination: string, overwrite: bool);
  public static method Move(source: string, destination: string);
end File;
