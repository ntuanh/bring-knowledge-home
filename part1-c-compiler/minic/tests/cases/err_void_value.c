// TEST: type errors - a void function result used as a value
// EXPECT-ERROR: 6:13: error: void value used where an int is required (function 'hello' returns void)
// EXPECT-ERROR: 7:16: error: void value used where an int is required
void hello() { print(1); }
int main() {
    int x = hello();
    return 1 + print(2);
}
