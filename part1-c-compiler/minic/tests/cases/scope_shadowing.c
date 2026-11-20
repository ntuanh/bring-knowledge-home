// TEST: an inner block may shadow an outer variable; the outer one is untouched
// EXPECT-STDOUT: 2|3|1
// EXPECT-EXIT: 1
// EXPECT-IR-OPT: return 1
int main() {
    int a = 1;
    {
        int a = 2;
        print(a);
        {
            int a = 3;
            print(a);
        }
    }
    print(a);
    return a;
}
