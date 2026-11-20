// TEST: type errors - return with a value in void, return without one in int
// EXPECT-ERROR: 4:12: error: return with a value in void function 'f'
// EXPECT-ERROR: 5:11: error: return without a value in int function 'g'
void f() { return 1; }
int g() { return; }
int main() { return 0; }
