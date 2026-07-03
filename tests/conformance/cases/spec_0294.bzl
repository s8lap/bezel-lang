Literal         ::= IntLiteral | FloatLiteral | CharLiteral
                  | StringLiteral | InterpString
                  | "true" | "false" | "null"
ConstExpr       ::= Expression           /* must be evaluable at compile time */
Constant        ::= "-"? (IntLiteral | FloatLiteral)
                  | CharLiteral
                  | StringLiteral | "true" | "false" | "null"
                  | QualifiedName        /* enum constant or const reference */
