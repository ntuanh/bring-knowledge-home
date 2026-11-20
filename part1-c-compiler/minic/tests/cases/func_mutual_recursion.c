// TEST: a function may be called before its definition (two-pass name lookup)
// EXPECT-STDOUT: 1|0
// EXPECT-EXIT: 1
// NO-GCC
int main() {
    print(is_even(10));
    print(is_even(7));
    return is_odd(3);
}
int is_even(int n) { if (n == 0) return 1; return is_odd(n - 1); }
int is_odd(int n) { if (n == 0) return 0; return is_even(n - 1); }
