// TEST: six parameters travel in edi, esi, edx, ecx, r8d, r9d in that order
// EXPECT-STDOUT: 123456|654321
// EXPECT-EXIT: 21
int digits(int a, int b, int c, int d, int e, int f) {
    return ((((a * 10 + b) * 10 + c) * 10 + d) * 10 + e) * 10 + f;
}
int sum6(int a, int b, int c, int d, int e, int f) {
    return a + b + c + d + e + f;
}
int main() {
    print(digits(1, 2, 3, 4, 5, 6));
    print(digits(6, 5, 4, 3, 2, 1));
    return sum6(1, 2, 3, 4, 5, sum6(1, 1, 1, 1, 1, 1));
}
