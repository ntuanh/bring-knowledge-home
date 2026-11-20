// TEST: x*1, x+0 and x*0 are simplified without knowing x
// EXPECT-STDOUT: 7|7|0
// EXPECT-EXIT: 7
// EXPECT-ASM-O1-NOT: imul
// EXPECT-ASM-O1-NOT: add
int id(int x) { return x; }
int main() {
    int x = id(7);
    print(x * 1);
    print(x + 0);
    print(x * 0);
    return 1 * x - 0;
}
