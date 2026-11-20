// TEST: all six comparison operators produce 0 or 1
// EXPECT-STDOUT: 1|0|1|1|0|1|0|1
// EXPECT-EXIT: 0
int main() {
    int a = 3;
    int b = 5;
    print(a < b);
    print(a > b);
    print(a <= 3);
    print(a != b);
    print(a == b);
    print(b >= 5);
    print(-1 > a);
    print(-b < -a);
    return 0;
}
