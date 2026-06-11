public method Process()
  var result := 0;
  do
    using var fs := new FileStream("data.txt");
    result := fs.ReadCount();
  end;     // fs disposed here
  Console.WriteLine($"count = {result}");
end Process;
