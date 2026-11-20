// TEST: while loops, including a nested loop and a loop that never runs
// EXPECT-STDOUT: 55|36|0
// EXPECT-EXIT: 10
int main() {
    int i = 1;
    int sum = 0;
    while (i <= 10) {
        sum = sum + i;
        i = i + 1;
    }
    print(sum);
    int count = 0;
    int a = 0;
    while (a < 6) {
        int b = 0;
        while (b < 6) { count = count + 1; b = b + 1; }
        a = a + 1;
    }
    print(count);
    int never = 0;
    while (0) never = 1;
    print(never);
    return i - 1;
}
