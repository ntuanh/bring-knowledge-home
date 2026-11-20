// TEST: line comments and block comments are skipped, also inside expressions
// EXPECT-EXIT: 7
/* a block
   comment */
int main() { // trailing comment
    return 3 /* inline */ + 4; /* another */
}
