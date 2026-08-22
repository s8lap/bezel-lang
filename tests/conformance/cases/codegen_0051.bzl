for var conn in pool do
  conn.Dispose();                  // ERROR: only owners dispose
end;
method Close(s: Stream)  s.Dispose();  end Close;              // ERROR: `s` is borrowed
