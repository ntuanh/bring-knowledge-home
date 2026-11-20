// TEST: a variable changed inside a loop must not be treated as a constant
// EXPECT-STDOUT: 10|45
// EXPECT-EXIT: 0
// EXPECT-ASM-O1: je
int main() {
    int i = 0;
    int total = 0;
    while (i < 10) {
        total = total + i;
        i = i + 1;
    }
    print(i);
    print(total);
    return 0;
}
