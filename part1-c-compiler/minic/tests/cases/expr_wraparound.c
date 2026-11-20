// TEST: 32-bit wrap-around is the same at run time (-O0) and when folded (-O1)
// EXPECT-STDOUT: -2147483648|2147483647|1
// EXPECT-EXIT: 0
int main() {
    int max = 2147483647;
    print(max + 1);
    print(max * 3 + 2);
    print(65536 * 65536 + 1);
    return 0;
}
