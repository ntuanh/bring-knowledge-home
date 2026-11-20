// TEST: recursive factorial and fibonacci
// EXPECT-STDOUT: 120|55
// EXPECT-EXIT: 24
int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}
int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
int main() {
    print(fact(5));
    print(fib(10));
    return fact(4);
}
