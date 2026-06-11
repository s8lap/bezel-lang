public method ProcessFile(filename: string)
  using var fs := new FileStream(filename);
  using var buffer := new Array<byte>(4096);
  var data := fs.ReadAll();
  // fs and buffer disposed here in reverse order
end ProcessFile;
