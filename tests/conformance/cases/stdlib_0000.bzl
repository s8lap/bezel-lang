public static class Console
  // Output
  public static method Write(value: string);
  public static method Write(value: Object);
  public static method WriteLine();
  public static method WriteLine(value: string);
  public static method WriteLine(value: Object);

  // Input
  public static method ReadLine(): string?;     // null at EOF
  public static method ReadKey(): char;

  // Streams
  public static readonly property In: TextReader;
  public static readonly property Out: TextWriter;
  public static readonly property Error: TextWriter;

  // Convenience
  public static method Clear();
end Console;
