// TEST: syntax error - the closing brace of main is missing
// EXPECT-ERROR: error: expected '}' before end of file
int main() {
    if (1) {
        return 1;
    return 0;
}
