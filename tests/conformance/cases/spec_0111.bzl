case delta of
  -10..-6 => "strongly down",
  ..<0    => "down",
  0       => "flat",
  _       => "up"
end;
