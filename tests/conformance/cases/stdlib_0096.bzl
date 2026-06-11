public abstract class TextReader
  public abstract method Read(): int;             // next char as int, or -1 at EOF
  public abstract method ReadLine(): string?;      // null at EOF
  public method ReadToEnd(): string;
  destructor;       // releases the underlying resource
end TextReader;

public abstract class TextWriter
  public abstract method Write(value: string);
  public abstract method Write(value: char);
  public method WriteLine();
  public method WriteLine(value: string);
  public abstract method Flush();
  destructor;       // flushes and releases the underlying resource
end TextWriter;
