// RUN: clang-driver -ccc-print-options input -Yunknown -m32 -arch ppc -djoined -A separate -Ajoined -Wp,one,two -Xarch_joined AndSeparate -sectalign 1 2 3 &> %t &&
// RUN: grep 'Option 0 - Name: "<input>", Values: {"input"}' %t &&
// RUN: grep 'Option 1 - Name: "<unknown>", Values: {"-Yunknown"}' %t &&
// RUN: grep 'Option 2 - Name: "-m32", Values: {}' %t &&
// RUN: grep 'Option 3 - Name: "-arch", Values: {"ppc"}' %t &&
// RUN: grep 'Option 4 - Name: "-d", Values: {"joined"}' %t &&
// RUN: grep 'Option 5 - Name: "-A", Values: {"separate"}' %t &&
// RUN: grep 'Option 6 - Name: "-A", Values: {"joined"}' %t &&
// RUN: grep 'Option 7 - Name: "-Wp,", Values: {"one", "two"}' %t &&
// RUN: grep 'Option 8 - Name: "-Xarch_", Values: {"joined", "AndSeparate"}' %t &&
// RUN: grep 'Option 9 - Name: "-sectalign", Values: {"1", "2", "3"}' %t &&

// RUN: true

