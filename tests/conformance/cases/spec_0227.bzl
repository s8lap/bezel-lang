public method ProcessFile(filename: string)
  using var fs := new FileStream(filename, FileMode.Open);
  using var buffer := new Array<byte>(4096, 0);   // sized ctor (stdlib §4.4)
  var data := fs.ReadAll();
  // fs and buffer disposed here in reverse order
end ProcessFile;
