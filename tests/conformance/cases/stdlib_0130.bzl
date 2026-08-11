public static class Directory
  public static method Exists(path: string): bool;
  public static method CreateDirectory(path: string);
  public static method Delete(path: string);                       // IOException if non-empty
  public static method Delete(path: string, recursive: bool);
  public static method GetFiles(path: string): owned Array<string>;
  public static method GetDirectories(path: string): owned Array<string>;
end Directory;

public static class Path
  public static method Combine(a: string, b: string): string;
  public static method GetFileName(path: string): string;
  public static method GetExtension(path: string): string;         // "" when none
  public static method GetFileNameWithoutExtension(path: string): string;
  public static method GetDirectoryName(path: string): string;
  public static method GetFullPath(path: string): string;
end Path;
