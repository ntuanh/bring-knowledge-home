// TEST: semantic errors - undefined function and wrong number of arguments
// EXPECT-ERROR: error: call to undefined function 'missing'
// EXPECT-ERROR: error: function 'add' expects 2 arguments but 1 was given
// EXPECT-ERROR: error: function 'add' expects 2 arguments but 3 were given
int add(int a, int b) { return a + b; }
int main() {
    missing(1);
    add(1);
    return add(1, 2, 3);
}
