// RUN: clang-cc -triple i386-unknown-unknown -fvisibility=default -emit-llvm -o %t %s &&
// RUN: grep '@g_com = common global i32 0' %t &&
// RUN: grep '@g_def = global i32 0' %t &&
// RUN: grep '@g_ext = external global i32' %t &&
// RUN: grep 'declare void @f_ext()' %t &&
// RUN: grep 'define i32 @f_def()' %t &&
// RUN: clang-cc -triple i386-unknown-unknown -fvisibility=protected -emit-llvm -o %t %s &&
// RUN: grep '@g_com = common protected global i32 0' %t &&
// RUN: grep '@g_def = protected global i32 0' %t &&
// RUN: grep '@g_ext = external global i32' %t &&
// RUN: grep 'declare void @f_ext()' %t &&
// RUN: grep 'define protected i32 @f_def()' %t &&
// RUN: clang-cc -triple i386-unknown-unknown -fvisibility=hidden -emit-llvm -o %t %s &&
// RUN: grep '@g_com = common hidden global i32 0' %t &&a
// RUN: grep '@g_def = hidden global i32 0' %t &&
// RUN: grep '@g_ext = external global i32' %t &&
// RUN: grep 'declare void @f_ext()' %t &&
// RUN: grep 'define hidden i32 @f_def()' %t &&
// RUN: true

int g_com;
int g_def = 0;
extern int g_ext;

extern void f_ext(void);

int f_def(void) {
  f_ext();
  return g_com + g_def + g_ext;
}