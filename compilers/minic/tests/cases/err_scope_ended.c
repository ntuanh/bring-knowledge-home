// TEST: semantic error - a variable is not visible after its block ends
// EXPECT-ERROR: 5:12: error: use of undeclared variable 'inner'
int main() {
    { int inner = 1; }
    return inner;
}
