Identifier      ::= IdentStart IdentCont*
IdentStart      ::= Letter | "_"
IdentCont       ::= IdentStart | Digit
Letter          ::= /any Unicode letter/
Digit           ::= "0".."9"
HexDigit        ::= Digit | "a".."f" | "A".."F"

IntLiteral      ::= DecLiteral | HexLiteral | BinLiteral
DecLiteral      ::= Digit ("_"* Digit)* IntSuffix?
HexLiteral      ::= "0x" HexDigit ("_"* HexDigit)* IntSuffix?
BinLiteral      ::= "0b" ("0" | "1") ("_"* ("0" | "1"))* IntSuffix?
IntSuffix       ::= "L" | "U" | "UL" | "LU"   /* case-insensitive */

FloatLiteral    ::= DigitGroup "." DigitGroup Exponent? FloatSuffix?
                  | DigitGroup Exponent FloatSuffix?
                  | DigitGroup FloatSuffix
DigitGroup      ::= Digit ("_"* Digit)*
Exponent        ::= ("e" | "E") ("+" | "-")? DigitGroup
FloatSuffix     ::= "f" | "F" | "m" | "M"     /* m -> decimal */

CharLiteral     ::= "'" CharContent "'"
StringLiteral   ::= '"' StringContent* '"'
                  | '@"' VerbatimContent* '"'
InterpString    ::= '$"' (InterpPart | StringContent)* '"'
                  | '$@"' (InterpPart | VerbatimContent)* '"'
InterpPart      ::= "{" Expression ("," "-"? IntLiteral)? (":" FormatSpec)? "}"
