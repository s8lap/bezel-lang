public method Resize(newSize: int)
  _buffer.Dispose();
  _buffer := new Array<byte>(newSize);
end Resize;
