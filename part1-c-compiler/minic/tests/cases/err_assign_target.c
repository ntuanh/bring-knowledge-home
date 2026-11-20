// TEST: semantic error - the left side of '=' must be a variable
// EXPECT-ERROR: 5:5: error: left side of '=' must be a variable
int main() {
    int a = 1;
    a + 1 = 5;
    return a;
}
