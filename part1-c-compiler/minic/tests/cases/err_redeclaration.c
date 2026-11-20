// TEST: semantic error - same name twice in one scope (a parameter counts)
// EXPECT-ERROR: 6:5: error: redeclaration of 'a' in the same scope
// EXPECT-ERROR: 10:5: error: redeclaration of 'p' in the same scope
int main() {
    int a = 1;
    int a = 2;
    return a;
}
int f(int p) {
    int p = 3;
    return p;
}
