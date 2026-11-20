// TEST: semantic error - two functions with the same name
// EXPECT-ERROR: 4:1: error: duplicate function 'f' (first defined at 3:1)
int f() { return 1; }
int f() { return 2; }
int main() { return f(); }
