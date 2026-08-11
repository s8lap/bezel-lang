public method ProcessLargeFile(filename: string)
  var result := 0;

  do
    using var fs := new FileStream(filename, FileMode.Open);
    var data := fs.ReadAll();
    result := ProcessData(data);
  end;
  // fs has been disposed here

  Console.WriteLine($"Result = {result}");
end ProcessLargeFile;
