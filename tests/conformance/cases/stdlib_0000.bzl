public static class Console
  // Output
  public static method Write(value: string);
  public static method Write(value: Object);
  public static method WriteLine();
  public static method WriteLine(value: string);
  public static method WriteLine(value: Object);
  // v0.165 (#1355): per-type Write/WriteLine overloads for bool and the numeric
  // primitives (same shape as StringBuilder.Append §3.1) — behavior =
  // Write(value.ToString()); no Object boxing on the path. TextWriter (§7.4) mirrors them.

  // Input
  public static method ReadLine(): string?;     // null at EOF
  public static method ReadKey(): char;         // blocks for one key; EndOfStreamException at end of input (v0.195; see below)
  public static method ReadKeyOrNull(): char?;  // v0.231 (§1 *OrNull tier): null at end of input; otherwise blocks for one key

  // Streams
  public static readonly property In: TextReader;
  public static readonly property Out: TextWriter;
  public static readonly property Error: TextWriter;

  // Convenience
  public static method Clear();
end Console;
