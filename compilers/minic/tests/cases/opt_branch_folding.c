// TEST: a constant condition removes the branch and the unreachable arm
// EXPECT-STDOUT: 2
// EXPECT-EXIT: 5
// EXPECT-ASM-O1-NOT: je
// EXPECT-ASM-O1-NOT: jmp
// EXPECT-IR-OPT-NOT: print(1)
int main() {
    int debug = 0;
    if (debug) {
        print(1);
    } else {
        print(2);
    }
    while (0) { print(3); }
    return 5;
}
