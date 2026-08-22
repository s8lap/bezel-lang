public method Process()
  var result := 0;
  do
    using var fs := new FileStream("data.txt", FileMode.Open);
    using var buffer := new Array<byte>(4096, 0);   // sized ctor (stdlib §4.4)
    result := fs.Read(buffer, 0, buffer.Count);     // bytes read (stdlib §7.1)
  end;     // fs and buffer disposed here
  Console.WriteLine($"count = {result}");
end Process;
