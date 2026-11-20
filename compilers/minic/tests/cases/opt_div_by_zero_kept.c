// TEST: division by zero is never folded or deleted: it must still trap at run time
// COMPILE-ONLY
// EXPECT-IR-OPT: 1 / 0
// EXPECT-IR-OPT: 5 % 0
// EXPECT-ASM-O1: idiv
int main() {
    int a = 1 / 0;
    int b = 5 % 0;
    return 0;
}
