// TEST: precedence of * / % over + -, and of arithmetic over comparisons
// EXPECT-STDOUT: 12|1|0|7|1
// EXPECT-EXIT: 0
int main() {
    int two = 2;
    print(two + 3 * 4 - 10 / 5);      // 2 + 12 - 2 = 12
    print(1 + 2 < 2 * 2);             // 3 < 4 = 1
    print(1 + 2 == 2 * 2);            // 3 == 4 = 0
    print((two + 3) % 3 + two * 3 - 1); // 2 + 6 - 1 = 7
    print(1 < 2 == 2 > 1);            // (1<2) == (2>1) = 1
    return 0;
}
