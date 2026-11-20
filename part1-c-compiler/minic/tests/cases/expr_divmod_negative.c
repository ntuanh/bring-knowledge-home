// TEST: / truncates toward zero and % takes the sign of the dividend, as in C
// EXPECT-STDOUT: -3|-1|-3|1|3|-1
// EXPECT-EXIT: 0
int main() {
    int a = -7;
    int b = 2;
    print(a / b);     // -3 (not -4)
    print(a % b);     // -1
    print(7 / -b);    // -3
    print(7 % -b);    // 1
    print(a / -b);    // 3
    print(a % -b);    // -1
    return 0;
}
