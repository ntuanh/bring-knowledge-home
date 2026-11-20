// TEST: lexical errors stop the compiler in the lexer stage
// EXPECT-ERROR: 4:15: error: unexpected character '@'
int main() {
    int a = 3 @ 4;
    return a;
}
