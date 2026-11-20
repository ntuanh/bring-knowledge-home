// TEST: nested if/else; an else belongs to the nearest if
// EXPECT-STDOUT: 2|20|30
// EXPECT-EXIT: 3
int classify(int x) {
    if (x < 0)
        return 1;
    else if (x == 0)
        return 2;
    else {
        if (x > 100) return 4;
        return 3;
    }
}
int main() {
    int x = 5;
    print(classify(0));
    if (x > 3)
        if (x > 10) print(10); else print(20);   // else pairs with the inner if
    if (x) { print(30); }
    return classify(50);
}
