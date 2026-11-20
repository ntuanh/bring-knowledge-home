// Expressions: precedence, associativity, unary operators, C division rules
int main() {
    int a = 7;
    int b = 2;
    print(a + b * 3);        // 13: * before +
    print(10 - 3 - 2);       // 5: left associative
    print(-a / b);           // -3: division truncates toward zero
    print(-a % b);           // -1: remainder has the sign of the dividend
    print(a > b == 1);       // 1: comparisons give 0 or 1
    print(!a + !0);          // 1
    return (a + b) * (a - b) % 16;   // 45 % 16 = 13
}
