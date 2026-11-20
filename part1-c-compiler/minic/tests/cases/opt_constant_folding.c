// TEST: 2*3+4 is computed at compile time, so -O1 needs no imul or add
// EXPECT-EXIT: 10
// EXPECT-ASM-O1-NOT: imul
// EXPECT-ASM-O1-NOT: add
// EXPECT-IR-OPT: return 10
int main() {
    return 2 * 3 + 4;
}
