public record Guid
  public static readonly property Empty: Guid;

  public static method NewGuid(): Guid;
  public static method Parse(s: string): Guid;
  public static method TryParse(s: string, out result: Guid): bool;

  public method ToString(): string;          // canonical form
  public method ToString(format: string): string;   // "N", "D", "B", "P"
  public method ToByteArray(): owned Array<byte>;
end Guid;
