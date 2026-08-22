public abstract class TextReader
  public abstract method Read(): int;             // next char as int, or -1 at EOF
  public abstract method ReadLine(): string?;      // null at EOF
  public method ReadToEnd(): string;
  destructor;       // releases the underlying resource
end TextReader;

public abstract class TextWriter
  public abstract method Write(value: string);
  public abstract method Write(value: char);
  public method Write(value: Object);            // fixed base impl: Write(value.ToString()) (v0.165, #1355)
  public method WriteLine();
  public method WriteLine(value: string);
  public method WriteLine(value: Object);
  // v0.165 (#1355): per-type Write/WriteLine overloads for bool and the numeric
  // primitives — the §2.1/§3.1 family; behavior = Write(value.ToString()); fixed
  // base implementations over the abstract Write(string). (Block updated v0.195.)
  public abstract method Flush();
  destructor;       // flushes and releases the underlying resource
end TextWriter;
