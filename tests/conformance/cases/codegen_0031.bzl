public abstract class Stream
  abstract readonly property CanRead, CanWrite, CanSeek: bool;
  abstract method Read(buffer: Array<byte>, offset: int, count: int): int;
  abstract method Write(buffer: Array<byte>, offset: int, count: int);
  method Read(buffer: Span<byte>): int;                 // fixed base implementation
  method Write(buffer: ReadOnlySpan<byte>);
  abstract method Flush();   abstract method Seek(offset: long, origin: SeekOrigin): long;
  abstract readonly property Length: long;   abstract property Position: long;
  async virtual method ReadAsync(buffer: Array<byte>, offset: int, count: int, token := ...): Task<int>;
  async virtual method WriteAsync(buffer: Array<byte>, offset: int, count: int, token := ...): Task;
  method ReadAll(): owned Array<byte>;
  async method ReadAllAsync(token := CancellationToken.None): OwningTask<Array<byte>>;
end
public enum SeekOrigin: byte  Begin, Current, End;  end
public class FileStream: Stream
  constructor(path: string, mode: FileMode);            // implies FileAccess.ReadWrite
  constructor(path: string, mode: FileMode, access: FileAccess);
  public readonly property Path: string;   destructor;  // flushes + closes
end
public enum FileMode: byte    CreateNew, Create, Open, OpenOrCreate, Truncate, Append;  end
public enum FileAccess: byte  Read, Write, ReadWrite;  end
public static class File
  ReadAllText(path: string): string;              WriteAllText(path: string, contents: string);
  ReadAllBytes(path: string): owned Array<byte>;  WriteAllBytes(path: string, contents: Array<byte>);
  WriteAllBytes(path: string, contents: ReadOnlySpan<byte>);
  ReadAllLines(path: string): owned Array<string>;  WriteAllLines(path: string, lines: IEnumerable<string>);
  async ReadAllTextAsync(path: string, token := ...): Task<string>;
  async WriteAllTextAsync(path: string, contents: string, token := ...): Task;
  Exists(path: string): bool;   Delete(path: string);
  Copy(source: string, destination: string, overwrite: bool);   Move(source: string, destination: string);
end
public abstract class TextReader          // Read(): int (next char, -1 at EOF); ReadLine(): string? (null
  method ReadToEnd(): string;             //   at EOF); ReadToEnd(): string; destructor
end
public abstract class TextWriter          // Write(value: string|char|Object); WriteLine() and its string,
  abstract method Flush();                //   Object and per-primitive overloads; Flush(); destructor
end
public static class Directory
  Exists(path: string): bool;   CreateDirectory(path: string);
  Delete(path: string);         Delete(path: string, recursive: bool);
  GetFiles(path: string): owned Array<string>;   GetDirectories(path: string): owned Array<string>;
end
public static class Path                  // exactly six members
  Combine(a: string, b: string): string;   GetFileName(path): string;   GetExtension(path): string;
  GetFileNameWithoutExtension(path): string;   GetDirectoryName(path): string;   GetFullPath(path): string;
end
