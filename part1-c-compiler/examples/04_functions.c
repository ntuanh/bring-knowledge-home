// Functions: recursion, the calling convention, six register arguments
int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}

int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int weighted(int a, int b, int c, int d, int e, int f) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

void show(int label, int value) {
    print(label);
    print(value);
}

int main() {
    show(1, fact(5));                     // 120
    show(2, fib(10));                     // 55
    show(3, weighted(1, 1, 1, 1, 1, 1));  // 21
    return fact(4);                       // exit code 24
}
