// TEST: all semantic errors are reported in one run, not just the first
// EXPECT-ERROR: use of undeclared variable 'a'
// EXPECT-ERROR: use of undeclared variable 'b'
// EXPECT-ERROR: call to undefined function 'nope'
// EXPECT-ERROR: return without a value in int function 'main'
int main() {
    a = 1;
    b = nope();
    return;
}
