// TEST: constants are propagated through variables and the dead stores removed
// EXPECT-EXIT: 50
// EXPECT-IR-OPT: return 50
// EXPECT-IR-OPT-NOT: a =
// EXPECT-ASM-O1-NOT: imul
int main() {
    int a = 10;
    int b = 20;
    int unused = a * 99;
    return a + b * 2;
}
