// RUN: clang-cc %s -fsyntax-only -verify
int f(int a) { } // expected-note {{previous definition is here}}
int f(int);
int f(int a) { } // expected-error {{redefinition of 'f'}}

// <rdar://problem/6097326>
int foo(x) {
  return 0;
}
int x = 1;
