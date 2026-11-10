module Files.DirList;

import System.IO;

public class Program
  public static method Main()
    const dir := "/var/tmp/bezel-scratch/ergo/t23dir";
    Directory.CreateDirectory(dir);
    File.WriteAllText(Path.Combine(dir, "b.txt"), "abc");
    File.WriteAllText(Path.Combine(dir, "a.log"), "0123456789");
    File.WriteAllText(Path.Combine(dir, "c.txt"), "");
    var files := Directory.GetFiles(dir);
    files.Sort();
    for var f in files do
      using var fs := new FileStream(f, FileMode.Open, FileAccess.Read);
      Console.WriteLine($"{Path.GetFileName(f)} {fs.Length}");
    end;
    var txt := files.Where(lambda(f) => Path.GetExtension(f) = ".txt").Count();
    Console.WriteLine($"txt files: {txt}");
    Directory.Delete(dir, recursive := true);
    Console.WriteLine($"exists: {Directory.Exists(dir)}");
  end Main;
end Program;
