public class FileStream: Stream
  public constructor(path: string, mode: FileMode);
  public constructor(path: string, mode: FileMode, access: FileAccess);
  // Stream operations plus file-specific:
  public readonly property Path: string;
  destructor;    // flushes buffered writes and closes the OS file handle
end FileStream;

public enum FileMode: byte
  CreateNew, Create, Open, OpenOrCreate, Truncate, Append;
end FileMode;

public enum FileAccess: byte
  Read, Write, ReadWrite;
end FileAccess;
