// TEST: semantic error - use of a variable that was never declared
// EXPECT-ERROR: 4:12: error: use of undeclared variable 'y'
int main() {
    return y + 1;
}
