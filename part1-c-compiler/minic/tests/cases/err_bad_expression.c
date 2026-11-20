// TEST: syntax error - an operator with no right operand
// EXPECT-ERROR: 4:16: error: expected an expression, found ';'
int main() {
    return 1 + ;
}
