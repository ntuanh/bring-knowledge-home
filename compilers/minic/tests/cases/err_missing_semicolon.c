// TEST: syntax error - a missing ';' is reported at the end of the statement
// EXPECT-ERROR: err_missing_semicolon.c:4:15: error: expected ';' before 'return'
int main() {
    int a = 10
    return a;
}
