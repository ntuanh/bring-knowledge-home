// TEST: unary minus, unary plus and logical not, including nesting
// EXPECT-STDOUT: -5|5|1|0|3|-6|1
// EXPECT-EXIT: 0
int main() {
    int x = 5;
    print(-x);
    print(-(-x));
    print(!0);
    print(!x);
    print(- -3);
    print(-x - 1);    // (-x) - 1
    print(!!x);
    return +0;
}
