// TEST: the milestone program; * binds tighter than +, so 10 + 20*2 = 50
// EXPECT-EXIT: 50
int main() {
    int a = 10;
    int b = 20;
    return a + b * 2;
}
