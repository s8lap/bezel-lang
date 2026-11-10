# t22-binary

*Area:* files & I/O

Write the int32 values 1, 256, -2, 1000000 little-endian to a binary file with a FileStream, then read the file back in 4-byte chunks, decode each value and print them and their sum (`1 256 -2 1000000`, `sum 1000255`) and the file length (`16 bytes`).
