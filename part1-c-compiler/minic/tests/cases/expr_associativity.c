// TEST: - and / are left associative; = is right associative
// EXPECT-STDOUT: 5|2|7|7
// EXPECT-EXIT: 0
int main() {
    int a;
    int b;
    int hundred = 100;
    print(10 - 3 - 2);        // (10 - 3) - 2 = 5, not 10 - (3 - 2) = 9
    print(hundred / 10 / 5);  // (100 / 10) / 5 = 2, not 100 / (10 / 5) = 50
    a = b = 7;                // a = (b = 7)
    print(a);
    print(b);
    return 0;
}
